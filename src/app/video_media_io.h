#pragma once

#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_extractor.h"

namespace VideoMediaIo
{

using GenerationCheck = bool (*)(uint32_t generation);

struct Context
{
    FILE *file = nullptr;
    uint32_t size = 0U;
    uint32_t generation = 0U;
    GenerationCheck generation_check = nullptr;

    uint64_t read_bytes = 0ULL;
    uint64_t read_us_total = 0ULL;
    uint32_t read_us_max = 0U;
    uint32_t read_calls = 0U;
    uint64_t gate_wait_us_total = 0ULL;
    uint32_t gate_wait_count = 0U;

    // R.40.2.3 Video I/O contention/error diagnostics.
    uint32_t lock_retry_count = 0U;
    uint32_t lock_recoveries = 0U;
    uint32_t lock_fatal_timeouts = 0U;
    uint32_t read_errors = 0U;
    uint32_t seek_errors = 0U;
    uint32_t aggregate_read_requests = 0U;
};

void configure(Context *io, uint32_t generation, GenerationCheck generation_check);
esp_err_t open(const char *path, Context *io);
void close(Context *io);

int read_cb(void *buffer, uint32_t size, void *ctx);
int seek_cb(uint32_t position, void *ctx);
uint32_t size_cb(void *ctx);

esp_err_t map_extractor_error(esp_extractor_err_t err);

} // namespace VideoMediaIo
