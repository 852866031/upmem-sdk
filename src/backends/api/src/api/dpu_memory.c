/* Copyright 2020 UPMEM. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <dpu_transfer_matrix.h>
#include <dpu.h>
#include <dpu_api_verbose.h>
#include <dpu_types.h>
#include <dpu_error.h>
#include <dpu_log_utils.h>
#include <dpu_rank.h>
#include <dpu_management.h>
#include <dpu_program.h>
#include <dpu_memory.h>
#include <dpu_internals.h>
#include <dpu_attributes.h>
#include <dpu_thread_job.h>

#define IRAM_MASK (0x80000000u)
#define MRAM_MASK (0x08000000u)

#define IRAM_ALIGN (3u)
#define WRAM_ALIGN (2u)

#define ALIGN_MASK(align) (~((1u << (align)) - 1u))
#define IRAM_ALIGN_MASK ALIGN_MASK(IRAM_ALIGN)
#define WRAM_ALIGN_MASK ALIGN_MASK(WRAM_ALIGN)

#define MEMORY_SWITCH(address, iram_statement, mram_statement, wram_statement)                                                   \
    do {                                                                                                                         \
        if (((address)&IRAM_MASK) == IRAM_MASK) {                                                                                \
            iram_statement;                                                                                                      \
        } else if (((address)&MRAM_MASK) == MRAM_MASK) {                                                                         \
            mram_statement;                                                                                                      \
        } else {                                                                                                                 \
            wram_statement;                                                                                                      \
        }                                                                                                                        \
    } while (0)

#define UPDATE_MRAM_COPY_PARAMETERS(address, length)                                                                             \
    do {                                                                                                                         \
        (address) &= ~MRAM_MASK;                                                                                                 \
    } while (0)

#define UPDATE_WRAM_COPY_PARAMETERS(address, length)                                                                             \
    do {                                                                                                                         \
        (address) >>= WRAM_ALIGN;                                                                                                \
        (length) >>= WRAM_ALIGN;                                                                                                 \
    } while (0)

#define UPDATE_IRAM_COPY_PARAMETERS(address, length)                                                                             \
    do {                                                                                                                         \
        (address) = ((address) & ~IRAM_MASK) >> IRAM_ALIGN;                                                                      \
        (length) >>= IRAM_ALIGN;                                                                                                 \
    } while (0)

#define MEMORY_SWITCH_AND_UPDATE_COPY_PARAMETERS(address, length, iram_statement, mram_statement, wram_statement)                \
    do {                                                                                                                         \
        MEMORY_SWITCH((address), iram_statement; UPDATE_IRAM_COPY_PARAMETERS((address), (length)), mram_statement;               \
                      UPDATE_MRAM_COPY_PARAMETERS((address), (length));                                                          \
                      , wram_statement;                                                                                          \
                      UPDATE_WRAM_COPY_PARAMETERS((address), (length)));                                                         \
    } while (0)

#define CHECK_SYMBOL(symbol, symbol_offset, length)                                                                              \
    do {                                                                                                                         \
        dpu_mem_max_addr_t _address = (symbol).address + (symbol_offset);                                                        \
        if (((symbol_offset) + (length)) > (symbol).size) {                                                                      \
            LOG_FN(WARNING, "invalid symbol access (offset:%u + length:%u > size:%u)", symbol_offset, length, (symbol).size);    \
            return DPU_ERR_INVALID_SYMBOL_ACCESS;                                                                                \
        }                                                                                                                        \
        MEMORY_SWITCH(                                                                                                           \
            _address,                                                                                                            \
            if ((_address & ~IRAM_ALIGN_MASK) != 0) {                                                                            \
                LOG_FN(WARNING, "invalid iram access (offset:0x%x, address:0x%x)", symbol_offset, (symbol).address);             \
                return DPU_ERR_INVALID_IRAM_ACCESS;                                                                              \
            } if (((length) & ~IRAM_ALIGN_MASK) != 0) {                                                                          \
                LOG_FN(WARNING, "invalid iram access (length:0x%x)", length);                                                    \
                return DPU_ERR_INVALID_IRAM_ACCESS;                                                                              \
            },                                                                                                                   \
            /* All alignment are allowed */,                                                                                     \
            if ((_address & ~WRAM_ALIGN_MASK) != 0) {                                                                            \
                LOG_FN(WARNING, "invalid wram access (offset:0x%x, address:0x%x)", symbol_offset, (symbol).address);             \
                return DPU_ERR_INVALID_WRAM_ACCESS;                                                                              \
            } if (((length) & ~WRAM_ALIGN_MASK) != 0) {                                                                          \
                LOG_FN(WARNING, "invalid wram access (length:0x%x)", length);                                                    \
                return DPU_ERR_INVALID_WRAM_ACCESS;                                                                              \
            });                                                                                                                  \
    } while (0)

#define DPU_BROADCAST_SET_JOB_TYPE(job_type, address, length)                                                                    \
    do {                                                                                                                         \
        MEMORY_SWITCH_AND_UPDATE_COPY_PARAMETERS(address,                                                                        \
            length,                                                                                                              \
            (job_type) = DPU_THREAD_JOB_COPY_IRAM_TO_RANK,                                                                       \
            (job_type) = DPU_THREAD_JOB_COPY_MRAM_TO_RANK,                                                                       \
            (job_type) = DPU_THREAD_JOB_COPY_WRAM_TO_RANK);                                                                      \
    } while (0)

#define DPU_COPY_MATRIX_SET_JOB_TYPE(job_type, xfer, address, length)                                                            \
    do {                                                                                                                         \
        switch ((xfer)) {                                                                                                        \
            case DPU_XFER_TO_DPU:                                                                                                \
                MEMORY_SWITCH_AND_UPDATE_COPY_PARAMETERS(address,                                                                \
                    length,                                                                                                      \
                    (job_type) = DPU_THREAD_JOB_COPY_IRAM_TO_MATRIX,                                                             \
                    (job_type) = DPU_THREAD_JOB_COPY_MRAM_TO_MATRIX,                                                             \
                    (job_type) = DPU_THREAD_JOB_COPY_WRAM_TO_MATRIX);                                                            \
                break;                                                                                                           \
            case DPU_XFER_FROM_DPU:                                                                                              \
                MEMORY_SWITCH_AND_UPDATE_COPY_PARAMETERS(address,                                                                \
                    length,                                                                                                      \
                    (job_type) = DPU_THREAD_JOB_COPY_IRAM_FROM_MATRIX,                                                           \
                    (job_type) = DPU_THREAD_JOB_COPY_MRAM_FROM_MATRIX,                                                           \
                    (job_type) = DPU_THREAD_JOB_COPY_WRAM_FROM_MATRIX);                                                          \
                break;                                                                                                           \
            default:                                                                                                             \
                return DPU_ERR_INVALID_MEMORY_TRANSFER;                                                                          \
        }                                                                                                                        \
    } while (0)

#define SYNCHRONOUS_FLAGS(flags) ((DPU_XFER_ASYNC & flags) == 0)

static dpu_error_t
dpu_copy_symbol_dpu(struct dpu_t *dpu,
    struct dpu_symbol_t symbol,
    uint32_t symbol_offset,
    void *buffer,
    size_t length,
    dpu_xfer_t xfer,
    dpu_xfer_flags_t flags)
{
    if (!dpu->enabled) {
        return DPU_ERR_DPU_DISABLED;
    }
    CHECK_SYMBOL(symbol, symbol_offset, length);

    dpu_mem_max_addr_t address = symbol.address + symbol_offset;
    struct dpu_rank_t *rank = dpu_get_rank(dpu);
    dpu_error_t status = DPU_OK;

    enum dpu_thread_job_type job_type;
    DPU_COPY_MATRIX_SET_JOB_TYPE(job_type, xfer, address, length);

    uint32_t nr_jobs_per_rank;
    struct dpu_thread_job_sync sync;
    DPU_THREAD_JOB_GET_JOBS(&rank, 1, nr_jobs_per_rank, jobs, &sync, SYNCHRONOUS_FLAGS(flags), status);

    struct dpu_rank_t *rrank __attribute__((unused));
    struct dpu_thread_job *job;
    DPU_THREAD_JOB_SET_JOBS(&rank, rrank, 1, jobs, job, &sync, SYNCHRONOUS_FLAGS(flags), {
        job->type = job_type;
        dpu_transfer_matrix_clear_all(rank, &job->matrix);
        dpu_transfer_matrix_add_dpu(dpu, &job->matrix, buffer);
        job->matrix.offset = address;
        job->matrix.size = length;
    });

    status = dpu_thread_job_do_jobs(&rank, 1, nr_jobs_per_rank, jobs, SYNCHRONOUS_FLAGS(flags), &sync);

    return status;
}

static dpu_error_t
dpu_broadcast_to_symbol_for_ranks(struct dpu_rank_t **ranks,
    uint32_t nr_ranks,
    struct dpu_symbol_t symbol,
    uint32_t symbol_offset,
    const void *src,
    size_t length,
    dpu_xfer_flags_t flags)
{
    CHECK_SYMBOL(symbol, symbol_offset, length);

    dpu_error_t status = DPU_OK;
    dpu_mem_max_addr_t address = symbol.address + symbol_offset;

    enum dpu_thread_job_type job_type;
    DPU_BROADCAST_SET_JOB_TYPE(job_type, address, length);
    printf("[SDK] [QQQQQQQQ] done set job type\n");
    uint32_t nr_jobs_per_rank;
    struct dpu_thread_job_sync sync;
    printf("[SDK] [QQQQQQQQ] BEFORE GET JOBS\n");
    DPU_THREAD_JOB_GET_JOBS(ranks, nr_ranks, nr_jobs_per_rank, jobs, &sync, SYNCHRONOUS_FLAGS(flags), status);
    printf("[SDK] [QQQQQQQQ] GET JOBS DONE\n");
    struct dpu_rank_t *rank __attribute__((unused));
    struct dpu_thread_job *job;
    DPU_THREAD_JOB_SET_JOBS(ranks, rank, nr_ranks, jobs, job, &sync, SYNCHRONOUS_FLAGS(flags), {
        job->type = job_type;
        job->address = address;
        job->length = length;
        job->buffer = src;
    });
    printf("[SDK] [QQQQQQQQ] BEFORE DO JOBS\n");
    status = dpu_thread_job_do_jobs(ranks, nr_ranks, nr_jobs_per_rank, jobs, SYNCHRONOUS_FLAGS(flags), &sync);
printf("[SDK] [QQQQQQQQ] DONE JOB DO JOBSS\n");
    return status;
}

static const char *
dpu_transfer_to_string(dpu_xfer_t transfer)
{
    switch (transfer) {
        case DPU_XFER_TO_DPU:
            return "HOST_TO_DPU";
        case DPU_XFER_FROM_DPU:
            return "DPU_TO_HOST";
        default:
            return "UNKNOWN";
    }
}

static dpu_error_t
dpu_get_common_program(struct dpu_set_t *dpu_set, struct dpu_program_t **program)
{
    struct dpu_program_t *the_program = NULL;

    switch (dpu_set->kind) {
        case DPU_SET_RANKS:
            for (uint32_t each_rank = 0; each_rank < dpu_set->list.nr_ranks; ++each_rank) {
                struct dpu_rank_t *rank = dpu_set->list.ranks[each_rank];
                uint8_t nr_cis = rank->description->hw.topology.nr_of_control_interfaces;
                uint8_t nr_dpus_per_ci = rank->description->hw.topology.nr_of_dpus_per_control_interface;

                for (int each_ci = 0; each_ci < nr_cis; ++each_ci) {
                    for (int each_dpu = 0; each_dpu < nr_dpus_per_ci; ++each_dpu) {
                        struct dpu_t *dpu = DPU_GET_UNSAFE(rank, each_ci, each_dpu);

                        if (!dpu_is_enabled(dpu)) {
                            continue;
                        }

                        struct dpu_program_t *dpu_program = dpu_get_program(dpu);

                        if (the_program == NULL) {
                            the_program = dpu_program;
                        }

                        if (the_program != dpu_program) {
                            return DPU_ERR_DIFFERENT_DPU_PROGRAMS;
                        }
                    }
                }
            }
            break;
        case DPU_SET_DPU:
            the_program = dpu_get_program(dpu_set->dpu);
            break;
        default:
            return DPU_ERR_INTERNAL;
    }
    if (the_program == NULL) {
        return DPU_ERR_NO_PROGRAM_LOADED;
    }

    *program = the_program;
    return DPU_OK;
}

__API_SYMBOL__ dpu_error_t
dpu_get_symbol(struct dpu_program_t *program, const char *symbol_name, struct dpu_symbol_t *symbol)
{
    LOG_FN(DEBUG, "\"%s\"", symbol_name);

    dpu_error_t status = DPU_OK;

    uint32_t nr_symbols = program->symbols->nr_symbols;

    for (uint32_t each_symbol = 0; each_symbol < nr_symbols; ++each_symbol) {
        dpu_elf_symbol_t *elf_symbol = program->symbols->map + each_symbol;
        if (strcmp(symbol_name, elf_symbol->name) == 0) {
            symbol->address = elf_symbol->value;
            symbol->size = elf_symbol->size;
            goto end;
        }
    }

    status = DPU_ERR_UNKNOWN_SYMBOL;

end:
    return status;
}

__API_SYMBOL__ dpu_error_t
dpu_broadcast_to(struct dpu_set_t dpu_set,
    const char *symbol_name,
    uint32_t symbol_offset,
    const void *src,
    size_t length,
    dpu_xfer_flags_t flags)
{
    LOG_FN(DEBUG, "%s, %d, %zd, 0x%x", symbol_name, symbol_offset, length, flags);
    dpu_error_t status;
    struct dpu_program_t *program;
    struct dpu_symbol_t symbol;
    printf("[SDK] BEFORE GET COMMON PROGRAM\n");
    if ((status = dpu_get_common_program(&dpu_set, &program)) != DPU_OK) {
        return status;
    }
    printf("[SDK] BEFORE GET SYMBOL\n");
    if ((status = dpu_get_symbol(program, symbol_name, &symbol)) != DPU_OK) {
        return status;
    }
    printf("[SDK] BEFORE BROADCAST TO SYMBOL\n");
    return dpu_broadcast_to_symbol(dpu_set, symbol, symbol_offset, src, length, flags);
}

__API_SYMBOL__ dpu_error_t
dpu_broadcast_to_symbol(struct dpu_set_t dpu_set,
    struct dpu_symbol_t symbol,
    uint32_t symbol_offset,
    const void *src,
    size_t length,
    dpu_xfer_flags_t flags)
{
    LOG_FN(DEBUG, "0x%08x, %d, %d, %zd, 0x%x", symbol.address, symbol.size, symbol_offset, length, flags);
    dpu_error_t status = DPU_OK;
    printf("[SDK] [SSSSSS] IN BROADCAST TO SYMBOL\n");
    switch (dpu_set.kind) {
        case DPU_SET_RANKS:
        printf("[SDK] [SSSSSS] BEFORE BROADCAST TO SYMBOL FOR RANKS\n");
            status = dpu_broadcast_to_symbol_for_ranks(
                dpu_set.list.ranks, dpu_set.list.nr_ranks, symbol, symbol_offset, src, length, flags);
            printf("[SDK] [SSSSSS] DONE BROADCAST TO SYMBOL FOR RANKS\n");
            break;
        case DPU_SET_DPU:
        printf("[SDK] [SSSSSS] BEFORE COPY SYMBOL DPU\n");
            status = dpu_copy_symbol_dpu(dpu_set.dpu, symbol, symbol_offset, (void *)src, length, DPU_XFER_TO_DPU, flags);
            printf("[SDK] [SSSSSS] DONE COPY SYMBOL DPU\n");
            break;
        default:
            printf("[SDK] [SSSSSS] ERROR INTERNAL\n");
            return DPU_ERR_INTERNAL;
    }

    return status;
}

__API_SYMBOL__ dpu_error_t
dpu_copy_to(struct dpu_set_t dpu_set, const char *symbol_name, uint32_t symbol_offset, const void *src, size_t length)
{
    LOG_FN(DEBUG, "\"%s\", %d, %p, %zd)", symbol_name, symbol_offset, src, length);

    dpu_error_t status;
    struct dpu_program_t *program;
    struct dpu_symbol_t symbol;

    if ((status = dpu_get_common_program(&dpu_set, &program)) != DPU_OK) {
        return status;
    }

    if ((status = dpu_get_symbol(program, symbol_name, &symbol)) != DPU_OK) {
        return status;
    }

    return dpu_broadcast_to_symbol(dpu_set, symbol, symbol_offset, src, length, DPU_XFER_DEFAULT);
}

__API_SYMBOL__ dpu_error_t
dpu_copy_from(struct dpu_set_t dpu_set, const char *symbol_name, uint32_t symbol_offset, void *dst, size_t length)
{
    LOG_FN(DEBUG, "\"%s\", %d, %p, %zd)", symbol_name, symbol_offset, dst, length);

    if (dpu_set.kind != DPU_SET_DPU) {
        return dpu_set.kind == DPU_SET_RANKS ? DPU_ERR_INVALID_DPU_SET : DPU_ERR_INTERNAL;
    }

    struct dpu_t *dpu = dpu_set.dpu;
    dpu_error_t status = DPU_OK;

    struct dpu_symbol_t symbol;
    struct dpu_program_t *program;

    if ((program = dpu_get_program(dpu)) == NULL) {
        return DPU_ERR_NO_PROGRAM_LOADED;
    }

    if ((status = dpu_get_symbol(program, symbol_name, &symbol)) != DPU_OK) {
        return status;
    }

    return dpu_copy_symbol_dpu(dpu_set.dpu, symbol, symbol_offset, dst, length, DPU_XFER_FROM_DPU, DPU_XFER_DEFAULT);
}

__API_SYMBOL__ dpu_error_t
dpu_copy_to_symbol(struct dpu_set_t dpu_set, struct dpu_symbol_t symbol, uint32_t symbol_offset, const void *src, size_t length)
{
    LOG_FN(DEBUG, "0x%08x, %d, %d, %p, %zd)", symbol.address, symbol.size, symbol_offset, src, length);

    return dpu_broadcast_to_symbol(dpu_set, symbol, symbol_offset, src, length, DPU_XFER_DEFAULT);
}

__API_SYMBOL__ dpu_error_t
dpu_copy_from_symbol(struct dpu_set_t dpu_set, struct dpu_symbol_t symbol, uint32_t symbol_offset, void *dst, size_t length)
{
    LOG_FN(DEBUG, "0x%08x, %d, %d, %p, %zd)", symbol.address, symbol.size, symbol_offset, dst, length);

    if (dpu_set.kind != DPU_SET_DPU) {
        return dpu_set.kind == DPU_SET_RANKS ? DPU_ERR_INVALID_DPU_SET : DPU_ERR_INTERNAL;
    }

    return dpu_copy_symbol_dpu(dpu_set.dpu, symbol, symbol_offset, dst, length, DPU_XFER_FROM_DPU, DPU_XFER_DEFAULT);
}

struct dpu_transfer_matrix *
dpu_get_transfer_matrix(struct dpu_rank_t *rank)
{
    if (rank->api.callback_tid_set && rank->api.callback_tid == pthread_self()) {
        return &rank->api.callback_matrix;
    } else {
        return &rank->api.matrix;
    }
}

__API_SYMBOL__ dpu_error_t
dpu_prepare_xfer(struct dpu_set_t dpu_set, void *buffer)
{
    LOG_FN(DEBUG, "%p", buffer);

    switch (dpu_set.kind) {
        case DPU_SET_RANKS:
            for (uint32_t each_rank = 0; each_rank < dpu_set.list.nr_ranks; ++each_rank) {
                struct dpu_rank_t *rank = dpu_set.list.ranks[each_rank];
                uint8_t nr_cis = rank->description->hw.topology.nr_of_control_interfaces;
                uint8_t nr_dpus_per_ci = rank->description->hw.topology.nr_of_dpus_per_control_interface;

                for (uint8_t each_ci = 0; each_ci < nr_cis; ++each_ci) {
                    for (uint8_t each_dpu = 0; each_dpu < nr_dpus_per_ci; ++each_dpu) {
                        struct dpu_t *dpu = DPU_GET_UNSAFE(rank, each_ci, each_dpu);

                        if (!dpu_is_enabled(dpu)) {
                            continue;
                        }

                        dpu_transfer_matrix_add_dpu(dpu, dpu_get_transfer_matrix(rank), buffer);
                    }
                }
            }

            break;
        case DPU_SET_DPU: {
            struct dpu_t *dpu = dpu_set.dpu;

            if (!dpu_is_enabled(dpu)) {
                return DPU_ERR_DPU_DISABLED;
            }

            dpu_transfer_matrix_add_dpu(dpu, dpu_get_transfer_matrix(dpu_get_rank(dpu)), buffer);

            break;
        }
        default:
            return DPU_ERR_INTERNAL;
    }

    return DPU_OK;
}

__API_SYMBOL__ dpu_error_t
dpu_push_xfer(struct dpu_set_t dpu_set,
    dpu_xfer_t xfer,
    const char *symbol_name,
    uint32_t symbol_offset,
    size_t length,
    dpu_xfer_flags_t flags)
{
    LOG_FN(DEBUG, "%s, %s, %d, %zd, 0x%x", dpu_transfer_to_string(xfer), symbol_name, symbol_offset, length, flags);

    dpu_error_t status;
    struct dpu_program_t *program;
    struct dpu_symbol_t symbol;

    if ((status = dpu_get_common_program(&dpu_set, &program)) != DPU_OK) {
        return status;
    }

    if ((status = dpu_get_symbol(program, symbol_name, &symbol)) != DPU_OK) {
        return status;
    }

    return dpu_push_xfer_symbol(dpu_set, xfer, symbol, symbol_offset, length, flags);
}

__API_SYMBOL__ dpu_error_t
dpu_push_xfer_symbol(struct dpu_set_t dpu_set,
    dpu_xfer_t xfer,
    struct dpu_symbol_t symbol,
    uint32_t symbol_offset,
    size_t length,
    dpu_xfer_flags_t flags)
{
    LOG_FN(DEBUG,
        "%s, 0x%08x, %d, %d, %zd, 0x%x",
        dpu_transfer_to_string(xfer),
        symbol.address,
        symbol.size,
        symbol_offset,
        length,
        flags);

    dpu_error_t status = DPU_OK;
    dpu_mem_max_addr_t address = symbol.address + symbol_offset;

    switch (dpu_set.kind) {
        case DPU_SET_RANKS: {
            CHECK_SYMBOL(symbol, symbol_offset, length);

            uint32_t nr_ranks = dpu_set.list.nr_ranks;
            struct dpu_rank_t **ranks = dpu_set.list.ranks;

            enum dpu_thread_job_type job_type;
            DPU_COPY_MATRIX_SET_JOB_TYPE(job_type, xfer, address, length);

            uint32_t nr_jobs_per_rank;
            struct dpu_thread_job_sync sync;
            DPU_THREAD_JOB_GET_JOBS(ranks, nr_ranks, nr_jobs_per_rank, jobs, &sync, SYNCHRONOUS_FLAGS(flags), status);

            struct dpu_rank_t *rank;
            struct dpu_thread_job *job;
            DPU_THREAD_JOB_SET_JOBS(ranks, rank, nr_ranks, jobs, job, &sync, SYNCHRONOUS_FLAGS(flags), {
                dpu_transfer_matrix_copy(&job->matrix, dpu_get_transfer_matrix(rank));
                job->type = job_type;
                job->matrix.offset = address;
                job->matrix.size = length;
            });

            status = dpu_thread_job_do_jobs(ranks, nr_ranks, nr_jobs_per_rank, jobs, SYNCHRONOUS_FLAGS(flags), &sync);

        } break;
        case DPU_SET_DPU: {
            struct dpu_t *dpu = dpu_set.dpu;
            struct dpu_rank_t *rank = dpu_get_rank(dpu);
            uint8_t dpu_transfer_matrix_index = get_transfer_matrix_index(rank, dpu_get_slice_id(dpu), dpu_get_member_id(dpu));
            void *buffer = dpu_get_transfer_matrix(rank)->ptr[dpu_transfer_matrix_index];

            status = dpu_copy_symbol_dpu(dpu, symbol, symbol_offset, buffer, length, xfer, flags);

            break;
        }
        default:
            return DPU_ERR_INTERNAL;
    }

    if (flags != DPU_XFER_NO_RESET) {
        switch (dpu_set.kind) {
            case DPU_SET_RANKS:
                for (uint32_t each_rank = 0; each_rank < dpu_set.list.nr_ranks; ++each_rank) {
                    struct dpu_rank_t *rank = dpu_set.list.ranks[each_rank];
                    dpu_transfer_matrix_clear_all(rank, dpu_get_transfer_matrix(rank));
                }
                break;
            case DPU_SET_DPU: {
                struct dpu_t *dpu = dpu_set.dpu;
                dpu_transfer_matrix_clear_dpu(dpu, dpu_get_transfer_matrix(dpu_get_rank(dpu)));
                break;
            }
            default:
                return DPU_ERR_INTERNAL;
        }
    }

    return status;
}
