#define _POSIX_C_SOURCE 200809L

#include <vulkan/vulkan.h>

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

enum { MAX_SAMPLES = 200, COMPUTE_WORDS = 1024 * 1024, BUFFER_BYTES = 32 * 1024 * 1024 };

struct Buffer {
    VkBuffer handle;
    VkDeviceMemory memory;
    VkDeviceSize allocation_size;
    void *mapping;
    bool coherent;
};

struct Measurements {
    double wall_ms[MAX_SAMPLES];
    double gpu_ms[MAX_SAMPLES];
    int samples;
    bool has_gpu_timestamps;
};

struct Parameters {
    uint32_t count;
    uint32_t rounds;
};

static void require_vk(VkResult result, const char *operation) {
    if (result != VK_SUCCESS) {
        fprintf(stderr, "%s failed (VkResult %d)\n", operation, result);
        exit(2);
    }
}

static void require(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(2);
    }
}

static double elapsed_ms(struct timespec begin, struct timespec end) {
    return (double)(end.tv_sec - begin.tv_sec) * 1000.0 +
           (double)(end.tv_nsec - begin.tv_nsec) / 1000000.0;
}

static void json_string(const char *value) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            putchar('\\');
            putchar(*p);
        } else if (*p < 32) {
            printf("\\u%04x", (unsigned)*p);
        } else {
            putchar(*p);
        }
    }
    putchar('"');
}

static bool has_instance_extension(const char *name) {
    uint32_t count = 0;
    require_vk(vkEnumerateInstanceExtensionProperties(NULL, &count, NULL),
               "enumerate instance extension count");
    VkExtensionProperties *properties = calloc(count, sizeof(*properties));
    require(properties != NULL, "out of host memory");
    require_vk(vkEnumerateInstanceExtensionProperties(NULL, &count, properties),
               "enumerate instance extensions");
    bool found = false;
    for (uint32_t i = 0; i < count; i++)
        found |= strcmp(properties[i].extensionName, name) == 0;
    free(properties);
    return found;
}

static bool has_device_extension(VkPhysicalDevice gpu, const char *name) {
    uint32_t count = 0;
    require_vk(vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL),
               "enumerate device extension count");
    VkExtensionProperties *properties = calloc(count, sizeof(*properties));
    require(properties != NULL, "out of host memory");
    require_vk(vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, properties),
               "enumerate device extensions");
    bool found = false;
    for (uint32_t i = 0; i < count; i++)
        found |= strcmp(properties[i].extensionName, name) == 0;
    free(properties);
    return found;
}

static struct Buffer create_buffer(VkDevice device,
                                   const VkPhysicalDeviceMemoryProperties *memory,
                                   VkDeviceSize bytes) {
    struct Buffer buffer = {0};
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    require_vk(vkCreateBuffer(device, &info, NULL, &buffer.handle),
               "create buffer");
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);
    int selected = -1;
    for (uint32_t i = 0; i < memory->memoryTypeCount; ++i) {
        VkMemoryPropertyFlags flags = memory->memoryTypes[i].propertyFlags;
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            selected = (int)i;
            buffer.coherent = true;
            break;
        }
    }
    if (selected == -1) {
        for (uint32_t i = 0; i < memory->memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory->memoryTypes[i].propertyFlags &
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                selected = (int)i;
                break;
            }
        }
    }
    require(selected != -1, "no host-visible Vulkan buffer memory type");
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = (uint32_t)selected,
    };
    require_vk(vkAllocateMemory(device, &allocation, NULL, &buffer.memory),
               "allocate buffer memory");
    require_vk(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0),
               "bind buffer memory");
    require_vk(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0,
                           &buffer.mapping), "map buffer memory");
    buffer.allocation_size = requirements.size;
    return buffer;
}

static void sync_buffer(VkDevice device, const struct Buffer *buffer,
                        bool flush) {
    if (buffer->coherent)
        return;
    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = buffer->memory,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    require_vk(flush ? vkFlushMappedMemoryRanges(device, 1, &range)
                     : vkInvalidateMappedMemoryRanges(device, 1, &range),
               flush ? "flush host writes" : "invalidate host reads");
}

static void destroy_buffer(VkDevice device, struct Buffer *buffer) {
    vkUnmapMemory(device, buffer->memory);
    vkDestroyBuffer(device, buffer->handle, NULL);
    vkFreeMemory(device, buffer->memory, NULL);
}

static unsigned char *read_file(const char *filename, size_t *length) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror(filename);
        exit(2);
    }
    require(fseek(file, 0, SEEK_END) == 0, "seek SPIR-V file failed");
    long size = ftell(file);
    require(size > 0 && size < 16 * 1024 * 1024 && size % 4 == 0,
            "invalid SPIR-V file size");
    rewind(file);
    unsigned char *data = malloc((size_t)size);
    require(data != NULL, "allocate SPIR-V file failed");
    require(fread(data, 1, (size_t)size, file) == (size_t)size,
            "read SPIR-V file failed");
    fclose(file);
    *length = (size_t)size;
    return data;
}

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void print_stats(const char *name, const double *samples, int count) {
    double sorted[MAX_SAMPLES];
    memcpy(sorted, samples, (size_t)count * sizeof(double));
    qsort(sorted, (size_t)count, sizeof(double), compare_double);
    double median = count % 2 ? sorted[count / 2] :
                    (sorted[count / 2 - 1] + sorted[count / 2]) / 2.0;
    int p95 = (int)ceil((double)count * 0.95) - 1;
    int p99 = (int)ceil((double)count * 0.99) - 1;
    printf("\"%s\":{\"median_ms\":%.6f,\"p95_ms\":%.6f,"
           "\"p99_ms\":%.6f,\"samples_ms\":[",
           name, median, sorted[p95], sorted[p99]);
    for (int i = 0; i < count; ++i)
        printf("%s%.6f", i ? "," : "", samples[i]);
    printf("]}");
}

static uint32_t source_word(uint32_t index) {
    return index * 2654435761u ^ 0x9e3779b9u;
}

static uint32_t transform_word(uint32_t value, uint32_t rounds) {
    for (uint32_t j = 0; j < rounds; ++j)
        value = (value ^ (value >> 13)) * 1664525u + 1013904223u;
    return value;
}

static uint64_t hash_words(const uint32_t *values, size_t count) {
    uint64_t result = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < count; ++i) {
        result ^= values[i];
        result *= UINT64_C(1099511628211);
    }
    return result;
}

static void run_test(VkDevice device, VkQueue queue, VkCommandBuffer command,
                     VkQueryPool queries, uint32_t timestamp_bits,
                     double timestamp_period, VkPipeline pipeline,
                     VkPipelineLayout pipeline_layout, VkDescriptorSet descriptors,
                     VkBuffer source, VkBuffer destination, bool compute,
                     struct Parameters parameters, int warmups,
                     struct Measurements *measurements) {
    const int total = warmups + measurements->samples;
    for (int i = 0; i < total; ++i) {
        require_vk(vkResetCommandBuffer(command, 0), "reset command buffer");
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        require_vk(vkBeginCommandBuffer(command, &begin), "begin commands");
        if (queries) {
            vkCmdResetQueryPool(command, queries, 0, 2);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                queries, 0);
        }
        if (compute) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    pipeline_layout, 0, 1, &descriptors, 0, NULL);
            vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(parameters), &parameters);
            vkCmdDispatch(command, (parameters.count + 255) / 256, 1, 1);
        } else {
            VkBufferCopy region = {.srcOffset = 0, .dstOffset = 0,
                                   .size = BUFFER_BYTES};
            vkCmdCopyBuffer(command, source, destination, 1, &region);
        }
        VkMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = compute ? VK_ACCESS_SHADER_WRITE_BIT :
                                       VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        vkCmdPipelineBarrier(command, compute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT :
                                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0,
                             1, &barrier, 0, NULL, 0, NULL);
        if (queries)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                queries, 1);
        require_vk(vkEndCommandBuffer(command), "end commands");

        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &command,
        };
        struct timespec before, after;
        clock_gettime(CLOCK_MONOTONIC, &before);
        require_vk(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE),
                   "submit command buffer");
        require_vk(vkQueueWaitIdle(queue), "wait for GPU completion");
        clock_gettime(CLOCK_MONOTONIC, &after);
        if (i >= warmups)
            measurements->wall_ms[i - warmups] = elapsed_ms(before, after);
        if (queries) {
            uint64_t timestamps[2];
            require_vk(vkGetQueryPoolResults(device, queries, 0, 2,
                       sizeof(timestamps), timestamps, sizeof(uint64_t),
                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                       "read GPU timestamps");
            /* Venus may advertise timestamps yet return an end value before
             * the start value. Never report the wrapped subtraction as time. */
            bool backwards = timestamp_bits == 64 && timestamps[1] < timestamps[0];
            uint64_t delta = timestamps[1] - timestamps[0];
            if (timestamp_bits < 64)
                delta &= (UINT64_C(1) << timestamp_bits) - 1;
            double gpu_ms = (double)delta * timestamp_period / 1000000.0;
            double wall_ms = elapsed_ms(before, after);
            if (backwards || !isfinite(gpu_ms) ||
                gpu_ms > wall_ms * 10.0 + 1.0) {
                if (measurements->has_gpu_timestamps) {
                    fprintf(stderr, "GPU timestamps unreliable (%" PRIu64
                            " then %" PRIu64 "); omitting GPU-only timing\n",
                            timestamps[0], timestamps[1]);
                }
                measurements->has_gpu_timestamps = false;
            } else if (i >= warmups) {
                measurements->gpu_ms[i - warmups] = gpu_ms;
            }
        }
    }
}

static void print_measurement(const char *name,
                              const struct Measurements *results,
                              const char *checksum) {
    printf("\"%s\":{", name);
    print_stats("wall_submit_wait", results->wall_ms, results->samples);
    printf(",");
    if (results->has_gpu_timestamps)
        print_stats("gpu_timestamps", results->gpu_ms, results->samples);
    else
        printf("\"gpu_timestamps\":null");
    printf(",\"checksum\":\"%s\"}", checksum);
}

static int parse_number(const char *text, int min, int max, const char *name) {
    char *end = NULL;
    long result = strtol(text, &end, 10);
    if (*text == 0 || *end != 0 || result < min || result > max) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return (int)result;
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 6) {
        fprintf(stderr, "Usage: %s shader.spv [--samples N --warmups N]\n", argv[0]);
        return 2;
    }
    int samples = 20, warmups = 3;
    if (argc == 6) {
        require(strcmp(argv[2], "--samples") == 0 &&
                strcmp(argv[4], "--warmups") == 0,
                "use --samples N --warmups N in that order");
        samples = parse_number(argv[3], 1, MAX_SAMPLES, "samples");
        warmups = parse_number(argv[5], 0, 50, "warmups");
    }

    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "UTM native-guest Vulkan workload benchmark",
        .apiVersion = VK_API_VERSION_1_1,
    };
    const char *portability_instance = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    bool portability = has_instance_extension(portability_instance);
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0,
        .enabledExtensionCount = portability ? 1u : 0u,
        .ppEnabledExtensionNames = portability ? &portability_instance : NULL,
    };
    VkInstance instance;
    require_vk(vkCreateInstance(&instance_info, NULL, &instance),
               "create Vulkan instance");

    uint32_t gpu_count = 0;
    require_vk(vkEnumeratePhysicalDevices(instance, &gpu_count, NULL),
               "enumerate Vulkan devices");
    require(gpu_count > 0 && gpu_count <= 32, "no usable Vulkan GPU");
    VkPhysicalDevice gpus[32];
    require_vk(vkEnumeratePhysicalDevices(instance, &gpu_count, gpus),
               "get Vulkan devices");
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    uint32_t queue_family = UINT32_MAX, timestamp_bits = 0;
    VkPhysicalDeviceProperties properties;
    for (uint32_t i = 0; i < gpu_count && gpu == VK_NULL_HANDLE; ++i) {
        VkPhysicalDeviceProperties candidate;
        vkGetPhysicalDeviceProperties(gpus[i], &candidate);
        if (candidate.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
            candidate.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            continue; /* Never silently compare software llvmpipe with hardware. */
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &family_count, NULL);
        VkQueueFamilyProperties *families = calloc(family_count, sizeof(*families));
        require(families != NULL, "out of host memory");
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &family_count, families);
        for (uint32_t j = 0; j < family_count; ++j) {
            if (families[j].queueCount &&
                (families[j].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                gpu = gpus[i];
                queue_family = j;
                timestamp_bits = families[j].timestampValidBits;
                properties = candidate;
                break;
            }
        }
        free(families);
    }
    require(gpu != VK_NULL_HANDLE, "no accelerated Vulkan compute GPU found");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *portability_device = "VK_KHR_portability_subset";
    bool device_portability = has_device_extension(gpu, portability_device);
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = device_portability ? 1u : 0u,
        .ppEnabledExtensionNames = device_portability ? &portability_device : NULL,
    };
    VkDevice device;
    require_vk(vkCreateDevice(gpu, &device_info, NULL, &device),
               "create compute device");
    VkQueue queue;
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    struct Buffer source = create_buffer(device, &memory, BUFFER_BYTES);
    struct Buffer destination = create_buffer(device, &memory, BUFFER_BYTES);
    uint32_t *input = source.mapping;
    uint32_t *output = destination.mapping;
    for (uint32_t i = 0; i < BUFFER_BYTES / 4; ++i)
        input[i] = source_word(i);
    memset(output, 0, BUFFER_BYTES);
    sync_buffer(device, &source, true);
    sync_buffer(device, &destination, true);

    VkDescriptorSetLayoutBinding bindings[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
    };
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings,
    };
    VkDescriptorSetLayout descriptor_layout;
    require_vk(vkCreateDescriptorSetLayout(device, &layout_info, NULL,
                                            &descriptor_layout), "create descriptor layout");
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0, .size = sizeof(struct Parameters),
    };
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &descriptor_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    };
    VkPipelineLayout pipeline_layout;
    require_vk(vkCreatePipelineLayout(device, &pipeline_layout_info, NULL,
                                       &pipeline_layout), "create pipeline layout");
    size_t shader_size;
    unsigned char *shader = read_file(argv[1], &shader_size);
    uint64_t shader_hash = hash_words((const uint32_t *)shader,
                                      shader_size / sizeof(uint32_t));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader,
    };
    VkShaderModule shader_module;
    require_vk(vkCreateShaderModule(device, &shader_info, NULL, &shader_module),
               "create compute shader");
    free(shader);
    VkPipelineShaderStageCreateInfo stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = shader_module, .pName = "main",
    };
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = stage, .layout = pipeline_layout,
    };
    VkPipeline pipeline;
    require_vk(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                         NULL, &pipeline), "create compute pipeline");

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2,
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size,
    };
    VkDescriptorPool descriptor_pool;
    require_vk(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool),
               "create descriptor pool");
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1, .pSetLayouts = &descriptor_layout,
    };
    VkDescriptorSet descriptors;
    require_vk(vkAllocateDescriptorSets(device, &set_info, &descriptors),
               "allocate descriptor set");
    VkDescriptorBufferInfo buffer_info[2] = {
        {.buffer = source.handle, .offset = 0, .range = BUFFER_BYTES},
        {.buffer = destination.handle, .offset = 0, .range = BUFFER_BYTES},
    };
    VkWriteDescriptorSet writes[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptors, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &buffer_info[0]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptors, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &buffer_info[1]},
    };
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family,
    };
    VkCommandPool command_pool;
    require_vk(vkCreateCommandPool(device, &command_pool_info, NULL,
                                    &command_pool), "create command pool");
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command;
    require_vk(vkAllocateCommandBuffers(device, &command_info, &command),
               "allocate command buffer");

    VkQueryPool query_pool = VK_NULL_HANDLE;
    if (timestamp_bits) {
        VkQueryPoolCreateInfo query_info = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2,
        };
        require_vk(vkCreateQueryPool(device, &query_info, NULL, &query_pool),
                   "create timestamp query pool");
    }

    struct Measurements compute = {.samples = samples,
        .has_gpu_timestamps = query_pool != VK_NULL_HANDLE};
    struct Measurements copy = {.samples = samples,
        .has_gpu_timestamps = query_pool != VK_NULL_HANDLE};
    struct Parameters parameters = {.count = COMPUTE_WORDS, .rounds = 64};
    run_test(device, queue, command, query_pool, timestamp_bits,
             properties.limits.timestampPeriod, pipeline, pipeline_layout,
             descriptors, source.handle, destination.handle, true,
             parameters, warmups, &compute);
    sync_buffer(device, &destination, false);
    uint64_t expected_hash = UINT64_C(14695981039346656037);
    for (uint32_t i = 0; i < COMPUTE_WORDS; ++i) {
        uint32_t expected = transform_word(source_word(i), parameters.rounds);
        if (output[i] != expected) {
            fprintf(stderr, "compute checksum mismatch at word %u: %u != %u\n",
                    i, output[i], expected);
            return 3;
        }
        expected_hash ^= expected;
        expected_hash *= UINT64_C(1099511628211);
    }
    require(hash_words(output, COMPUTE_WORDS) == expected_hash,
            "compute aggregate checksum mismatch");
    char compute_hash[24];
    snprintf(compute_hash, sizeof(compute_hash), "%016" PRIx64, expected_hash);
    run_test(device, queue, command, query_pool, timestamp_bits,
             properties.limits.timestampPeriod, pipeline, pipeline_layout,
             descriptors, source.handle, destination.handle, false,
             parameters, warmups, &copy);
    sync_buffer(device, &destination, false);
    require(memcmp(input, output, BUFFER_BYTES) == 0,
            "32 MiB GPU buffer copy verification failed");
    char copy_hash[24];
    snprintf(copy_hash, sizeof(copy_hash), "%016" PRIx64,
             hash_words(output, BUFFER_BYTES / sizeof(uint32_t)));

    struct utsname system;
    require(uname(&system) == 0, "uname failed");
    printf("{\"environment\":{\"os\":");
    json_string(system.sysname);
    printf(",\"arch\":");
    json_string(system.machine);
    printf(",\"gpu\":");
    json_string(properties.deviceName);
    printf(",\"device_type\":%u,\"vendor_id\":%u,\"api_version\":\"%u.%u.%u\","
           "\"queue_family\":%u,\"timestamp_valid_bits\":%u,"
           "\"timestamp_period_ns\":%.3f,\"source_coherent\":%s,"
           "\"destination_coherent\":%s},",
           properties.deviceType, properties.vendorID,
           VK_API_VERSION_MAJOR(properties.apiVersion),
           VK_API_VERSION_MINOR(properties.apiVersion),
           VK_API_VERSION_PATCH(properties.apiVersion),
           queue_family, timestamp_bits, properties.limits.timestampPeriod,
           source.coherent ? "true" : "false",
           destination.coherent ? "true" : "false");
    printf("\"method\":{\"api\":\"Vulkan 1.1\",\"samples\":%d,\"warmups\":%d,"
           "\"compute_words\":%d,\"compute_rounds\":%u,"
           "\"buffer_copy_bytes\":%d,\"shader_fnv64\":\"%016" PRIx64 "\","
           "\"gpu_timestamps_note\":"
           "\"Device-reported timestamps; use wall_submit_wait for "
           "end-to-end host and VM latency comparisons\"},",
           samples, warmups, COMPUTE_WORDS, parameters.rounds,
           BUFFER_BYTES, shader_hash);
    printf("\"workloads\":{");
    print_measurement("compute_integer_mix", &compute, compute_hash);
    printf(",");
    print_measurement("copy_32mib", &copy, copy_hash);
    printf("}}\n");

    if (query_pool)
        vkDestroyQueryPool(device, query_pool, NULL);
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyShaderModule(device, shader_module, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, descriptor_layout, NULL);
    destroy_buffer(device, &destination);
    destroy_buffer(device, &source);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
