/*
 * Centaurean Density
 *
 * Copyright (c) 2013, Guillaume Voirin
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Centaurean nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * 06/12/13 20:28
 *
 * --------------
 * Lion algorithm
 * --------------
 *
 * Author(s)
 * Guillaume Voirin (https://github.com/gpnuma)
 *
 * Description
 * Multiform compression algorithm
 */

#include "kernel_lion_encode.h"
#include "kernel_lion_dictionary.h"
#include "kernel.h"
#include "memory_location.h"

DENSITY_FORCE_INLINE DENSITY_KERNEL_ENCODE_STATE exitProcess(density_lion_encode_state *state, DENSITY_LION_ENCODE_PROCESS process, DENSITY_KERNEL_ENCODE_STATE kernelEncodeState) {
    state->process = process;
    return kernelEncodeState;
}

/*DENSITY_FORCE_INLINE void density_lion_encode_prepare_new_signature(density_memory_location *restrict out, density_lion_encode_state *restrict state) {
    state->signaturesCount++;
    state->shift = 0;
    state->registerSignature = 0;
    state->signature = (density_lion_signature *) (out->pointer);

    out->pointer += sizeof(density_lion_signature);
}

DENSITY_FORCE_INLINE void density_lion_encode_flush_register_signature(density_lion_encode_state * state) {
    *(state->signature) = state->registerSignature;
}

DENSITY_FORCE_INLINE void density_lion_encode_push_to_signature(density_memory_location *restrict out, density_lion_encode_state *restrict state, uint64_t content, uint_fast8_t bits) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    state->registerSignature |= (content << state->shift);
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    state->registerSignature |= (content << ((56 - (state->shift & ~0x7)) + (state->shift & 0x7)));
#endif

    state->shift += bits;

    if (density_unlikely(state->shift & 0xFFFFFFFFFFFFFFC0llu)) {
        density_lion_encode_flush_register_signature(state);
        const uint8_t remainder = (uint_fast8_t) (state->shift & 0x3F);
        density_lion_encode_prepare_new_signature(out, state);
        if (remainder)
            density_lion_encode_push_to_signature(out, state, content >> (bits - remainder), remainder); //todo check big endian
    }
}*/

DENSITY_FORCE_INLINE DENSITY_KERNEL_ENCODE_STATE density_lion_encode_prepare_new_block(density_memory_location *restrict out, density_lion_encode_state *restrict state) {
    if (DENSITY_LION_ENCODE_MINIMUM_OUTPUT_LOOKAHEAD > out->available_bytes)
        return DENSITY_KERNEL_ENCODE_STATE_STALL_ON_OUTPUT;

    switch (state->signatureData.count) {
        case DENSITY_LION_PREFERRED_EFFICIENCY_CHECK_SIGNATURES:
            if (state->efficiencyChecked ^ 0x1) {
                state->efficiencyChecked = 1;
                return DENSITY_KERNEL_ENCODE_STATE_INFO_EFFICIENCY_CHECK;
            }
            break;
        case DENSITY_LION_PREFERRED_BLOCK_SIGNATURES:
            state->signatureData.count = 0;
            state->efficiencyChecked = 0;

#if DENSITY_ENABLE_PARALLELIZABLE_DECOMPRESSIBLE_OUTPUT == DENSITY_YES
            if (state->resetCycle)
                state->resetCycle--;
            else {
                density_lion_dictionary_reset(&state->dictionary);
                state->resetCycle = DENSITY_DICTIONARY_PREFERRED_RESET_CYCLE - 1;
            }
#endif

            return DENSITY_KERNEL_ENCODE_STATE_INFO_NEW_BLOCK;
        default:
            break;
    }
    DENSITY_KERNEL_ENCODE_PREPARE_NEW_SIGNATURE(out, &state->signatureData);

    return DENSITY_KERNEL_ENCODE_STATE_READY;
}

DENSITY_FORCE_INLINE density_lion_entropy_code density_lion_encode_fetch_form_rank_for_use(density_lion_encode_state *state, DENSITY_LION_FORM form) {
    density_lion_form_statistics *stats = &state->formStatistics[form];

    const uint8_t rank = stats->rank;
    if (density_unlikely(rank)) {
        density_lion_form_rank *rankCurrent = &state->formRanks[rank];
        density_lion_form_rank *rankUpper = &state->formRanks[rank - 1];
        if (density_unlikely(stats->usage > rankUpper->statistics->usage)) {
            density_lion_form_statistics *replaced = rankUpper->statistics;
            replaced->rank++;
            stats->rank--;
            rankUpper->statistics = stats;
            rankCurrent->statistics = replaced;
        }
        stats->usage++;
        return density_lion_form_entropy_codes[rank];
    } else {
        stats->usage++;
        return density_lion_form_entropy_codes[0];
    }
}

DENSITY_FORCE_INLINE void density_lion_encode_process_unigram(density_lion_encode_state *restrict state, density_kernel_signature* restrict signature, uint_fast8_t *restrict shift, const uint8_t unigram) {
    density_lion_dictionary_unigram_node* unigram_found = state->dictionary.unigramsIndex[unigram];
    if(density_likely(unigram_found)) {
        const uint8_t rank = unigram_found->rank;
        if(rank) {
            if(!(rank & 0xE0)) { // < 32
                const density_lion_entropy_code indexCode = density_lion_index_entropy_codes[rank];
                DENSITY_KERNEL_ENCODE_PUSH_TO_SIGNATURE_NO_CHECK(*signature, *shift, indexCode.value, indexCode.bitLength);
            }/* else {
            DENSITY_KERNEL_ENCODE_PUSH_TO_SIGNATURE(out, &state->signatureData, DENSITY_LION_SIGNATURE_FLAG_UNIGRAM_PLAIN, 1);
            *(out->pointer) = unigram;
            out->pointer++;
            }*/
            density_lion_dictionary_unigram_node* previous_unigram = unigram_found->previousUnigram;
            uint8_t previous_unigram_value = previous_unigram->unigram;
            previous_unigram->unigram = unigram;
            unigram_found->unigram = previous_unigram_value;
            state->dictionary.unigramsIndex[unigram] = previous_unigram;
            state->dictionary.unigramsIndex[previous_unigram_value] = unigram_found;
        } else {
            const density_lion_entropy_code indexCode = density_lion_index_entropy_codes[0];
            DENSITY_KERNEL_ENCODE_PUSH_TO_SIGNATURE_NO_CHECK(*signature, *shift, indexCode.value, indexCode.bitLength);
        }
    } else {
        density_lion_dictionary_unigram_node* new_unigram = &state->dictionary.unigramsPool[state->dictionary.nextAvailableUnigram];
        new_unigram->unigram = unigram;
        new_unigram->previousUnigram = state->dictionary.lastUnigramNode;
        new_unigram->rank = state->dictionary.nextAvailableUnigram++;
        state->dictionary.unigramsIndex[unigram] = new_unigram;
        state->dictionary.lastUnigramNode = new_unigram;
    }
}

DENSITY_FORCE_INLINE void density_lion_encode_process_bigram(density_memory_location *restrict out, density_lion_encode_state *restrict state, const uint16_t bigram) {
    density_kernel_signature signature = 0;
    uint_fast8_t shift = 0;
    const uint8_t unigram_a = (uint8_t) (bigram & 0xFF);

    density_lion_encode_process_unigram(state, &signature, &shift, unigram_a);
    if(shift) {
        const uint8_t unigram_b = (uint8_t) ((bigram >> 8) & 0xFF);
        uint_fast8_t oldShift = shift;
        density_lion_encode_process_unigram(state, &signature, &shift, unigram_b);
        if(oldShift != shift) {
            DENSITY_KERNEL_ENCODE_PUSH_TO_SIGNATURE(out, &state->signatureData, signature << 1, shift + 1);
            return;
        }
    }

    plain_output:
    DENSITY_KERNEL_ENCODE_PUSH_TO_SIGNATURE(out, &state->signatureData, DENSITY_LION_SIGNATURE_FLAG_BIGRAM_PLAIN, 1);
    *(uint16_t*)out->pointer = bigram;
    out->pointer += sizeof(uint16_t);
}

DENSITY_FORCE_INLINE void density_lion_encode_kernel(density_memory_location *restrict out, uint32_t *restrict hash, const uint32_t chunk, density_lion_encode_state *restrict state) {
    DENSITY_LION_HASH_ALGORITHM(*hash, DENSITY_LITTLE_ENDIAN_32(chunk));
    uint32_t *predictedChunk = &(state->dictionary.predictions[state->lastHash].next_chunk_prediction);

    density_kernel_signature_data* signatureData = &state->signatureData;

    if (*predictedChunk ^ chunk) {
        density_lion_dictionary_chunk_entry *found = &state->dictionary.chunks[*hash];
        uint32_t *found_a = &found->chunk_a;
        if (*found_a ^ chunk) {
            uint32_t *found_b = &found->chunk_b;
            if (*found_b ^ chunk) {
                const density_lion_entropy_code code = density_lion_encode_fetch_form_rank_for_use(state, DENSITY_LION_FORM_SECONDARY_ACCESS);
                DENSITY_KERNEL_ENCODE_PUSH_TO_SIGNATURE(out, signatureData, code.value, code.bitLength);

                const uint32_t chunk_rs8 = chunk >> 8;
                const uint32_t chunk_rs16 = chunk >> 16;
                const uint32_t chunk_rs24 = chunk >> 24;

                const uint16_t bigram_p = (uint16_t) ((state->lastChunk >> 24) | ((chunk & 0xFF) << 8));
                const uint16_t bigram_a = (uint16_t) (chunk & 0xFFFF);
                const uint16_t bigram_b = (uint16_t) (chunk_rs8 & 0xFFFF);
                const uint16_t bigram_c = (uint16_t) (chunk_rs16 & 0xFFFF);

                const uint8_t hash_p = (uint8_t) (((bigram_p * DENSITY_LION_HASH32_MULTIPLIER) >> (32 - DENSITY_LION_BIGRAM_HASH_BITS)));
                const uint8_t hash_a = (uint8_t) (((bigram_a * DENSITY_LION_HASH32_MULTIPLIER) >> (32 - DENSITY_LION_BIGRAM_HASH_BITS)));
                const uint8_t hash_b = (uint8_t) (((bigram_b * DENSITY_LION_HASH32_MULTIPLIER) >> (32 - DENSITY_LION_BIGRAM_HASH_BITS)));
                const uint8_t hash_c = (uint8_t) (((bigram_c * DENSITY_LION_HASH32_MULTIPLIER) >> (32 - DENSITY_LION_BIGRAM_HASH_BITS)));

                density_lion_dictionary_bigram_entry *bigram_entry_a = &state->dictionary.bigrams[hash_a];
                density_lion_dictionary_bigram_entry *bigram_entry_c = &state->dictionary.bigrams[hash_c];

                if (bigram_entry_a->bigram == bigram_a) {
                    DENSITY_KERNEL_ENCODE_PUSH_TO_SIGNATURE(out, signatureData, DENSITY_LION_SIGNATURE_FLAG_BIGRAM_DICTIONARY, 1);

                    *(out->pointer) = hash_a;
                    out->pointer++;
                } else {
                    DENSITY_KERNEL_ENCODE_PUSH_TO_SIGNATURE(out, signatureData, DENSITY_LION_SIGNATURE_FLAG_BIGRAM_SECONDARY, 1);

                    density_lion_encode_process_bigram(out, state, bigram_a);
                }
                if (bigram_entry_c->bigram == bigram_c) {
                    DENSITY_KERNEL_ENCODE_PUSH_TO_SIGNATURE(out, signatureData, DENSITY_LION_SIGNATURE_FLAG_BIGRAM_DICTIONARY, 1);

                    *(out->pointer) = hash_c;
                    out->pointer++;
                } else {
                    DENSITY_KERNEL_ENCODE_PUSH_TO_SIGNATURE(out, signatureData, DENSITY_LION_SIGNATURE_FLAG_BIGRAM_SECONDARY, 1);

                    density_lion_encode_process_bigram(out, state, bigram_c);
                }

                state->dictionary.bigrams[hash_p].bigram = bigram_p;
                bigram_entry_a->bigram = bigram_a;
                state->dictionary.bigrams[hash_b].bigram = bigram_b;
                bigram_entry_c->bigram = bigram_c;
            } else {
                const density_lion_entropy_code code = density_lion_encode_fetch_form_rank_for_use(state, DENSITY_LION_FORM_CHUNK_DICTIONARY_B);
                DENSITY_KERNEL_ENCODE_PUSH_TO_SIGNATURE(out, signatureData, code.value, code.bitLength);

                *(uint16_t *) (out->pointer) = DENSITY_LITTLE_ENDIAN_16(*hash);
                out->pointer += sizeof(uint16_t);
            }
            *found_b = *found_a;
            *found_a = chunk;
        } else {
            const density_lion_entropy_code code = density_lion_encode_fetch_form_rank_for_use(state, DENSITY_LION_FORM_CHUNK_DICTIONARY_A);
            DENSITY_KERNEL_ENCODE_PUSH_TO_SIGNATURE(out, signatureData, code.value, code.bitLength);

            *(uint16_t *) (out->pointer) = DENSITY_LITTLE_ENDIAN_16(*hash);
            out->pointer += sizeof(uint16_t);
        }
        *predictedChunk = chunk;
    } else {
        const density_lion_entropy_code code = density_lion_encode_fetch_form_rank_for_use(state, DENSITY_LION_FORM_CHUNK_PREDICTIONS);
        DENSITY_KERNEL_ENCODE_PUSH_TO_SIGNATURE(out, signatureData, code.value, code.bitLength);
    }

    state->lastHash = *hash;
    state->lastChunk = chunk;
}

DENSITY_FORCE_INLINE void density_lion_encode_process_chunk(uint64_t *restrict chunk, density_memory_location *restrict in, density_memory_location *restrict out, uint32_t *restrict hash, density_lion_encode_state *restrict state) {
    *chunk = *(uint64_t *) (in->pointer);

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    density_lion_encode_kernel(out, hash, (uint32_t) (*chunk & 0xFFFFFFFF), state);
#endif
    density_lion_encode_kernel(out, hash, (uint32_t) (*chunk >> 32), state);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    density_lion_encode_kernel(out, hash, (uint32_t) (*chunk & 0xFFFFFFFF), state);
#endif

    in->pointer += sizeof(uint64_t);
}

DENSITY_FORCE_INLINE void density_lion_encode_process_span(uint64_t *restrict chunk, density_memory_location *restrict in, density_memory_location *restrict out, uint32_t *restrict hash, density_lion_encode_state *restrict state) {
    density_lion_encode_process_chunk(chunk, in, out, hash, state);
    density_lion_encode_process_chunk(chunk, in, out, hash, state);
    density_lion_encode_process_chunk(chunk, in, out, hash, state);
    density_lion_encode_process_chunk(chunk, in, out, hash, state);
}

DENSITY_FORCE_INLINE void density_lion_encode_process_unit(uint64_t *restrict chunk, density_memory_location *restrict in, density_memory_location *restrict out, uint32_t *restrict hash, density_lion_encode_state *restrict state) {
    density_lion_encode_process_span(chunk, in, out, hash, state);
    density_lion_encode_process_span(chunk, in, out, hash, state);
}

DENSITY_FORCE_INLINE DENSITY_KERNEL_ENCODE_STATE density_lion_encode_init(density_lion_encode_state *state) {
    state->signatureData.count = 0;
    state->efficiencyChecked = 0;
    density_lion_dictionary_reset(&state->dictionary);

#if DENSITY_ENABLE_PARALLELIZABLE_DECOMPRESSIBLE_OUTPUT == DENSITY_YES
    state->resetCycle = DENSITY_DICTIONARY_PREFERRED_RESET_CYCLE - 1;
#endif

    state->formStatistics[DENSITY_LION_FORM_CHUNK_PREDICTIONS].usage = 0;
    state->formStatistics[DENSITY_LION_FORM_CHUNK_PREDICTIONS].rank = 3;
    state->formStatistics[DENSITY_LION_FORM_CHUNK_DICTIONARY_A].usage = 0;
    state->formStatistics[DENSITY_LION_FORM_CHUNK_DICTIONARY_A].rank = 1;
    state->formStatistics[DENSITY_LION_FORM_CHUNK_DICTIONARY_B].usage = 0;
    state->formStatistics[DENSITY_LION_FORM_CHUNK_DICTIONARY_B].rank = 2;
    state->formStatistics[DENSITY_LION_FORM_SECONDARY_ACCESS].usage = 0;
    state->formStatistics[DENSITY_LION_FORM_SECONDARY_ACCESS].rank = 0;

    state->formRanks[3].statistics = &state->formStatistics[DENSITY_LION_FORM_CHUNK_PREDICTIONS];
    state->formRanks[1].statistics = &state->formStatistics[DENSITY_LION_FORM_CHUNK_DICTIONARY_A];
    state->formRanks[2].statistics = &state->formStatistics[DENSITY_LION_FORM_CHUNK_DICTIONARY_B];
    state->formRanks[0].statistics = &state->formStatistics[DENSITY_LION_FORM_SECONDARY_ACCESS];

    state->lastHash = 0;
    state->lastChunk = 0;

    return exitProcess(state, DENSITY_LION_ENCODE_PROCESS_PREPARE_NEW_BLOCK, DENSITY_KERNEL_ENCODE_STATE_READY);
}

DENSITY_FORCE_INLINE DENSITY_KERNEL_ENCODE_STATE density_lion_encode_continue(density_memory_teleport *restrict in, density_memory_location *restrict out, density_lion_encode_state *restrict state, const density_bool flush) {
    DENSITY_KERNEL_ENCODE_STATE returnState;
    uint32_t hash;
    uint64_t chunk;
    density_byte *pointerOutBefore;
    density_memory_location *readMemoryLocation;

    // Dispatch
    switch (state->process) {
        case DENSITY_LION_ENCODE_PROCESS_PREPARE_NEW_BLOCK:
            goto prepare_new_block;
        case DENSITY_LION_ENCODE_PROCESS_READ_CHUNK:
            goto read_chunk;
        case DENSITY_LION_ENCODE_PROCESS_CHECK_SIGNATURE_STATE:
            goto check_signature_state;
        default:
            return DENSITY_KERNEL_ENCODE_STATE_ERROR;
    }

    // Prepare new block
    prepare_new_block:
    if ((returnState = density_lion_encode_prepare_new_block(out, state)))
        return exitProcess(state, DENSITY_LION_ENCODE_PROCESS_PREPARE_NEW_BLOCK, returnState);

    check_signature_state:
    if (DENSITY_LION_ENCODE_MINIMUM_OUTPUT_LOOKAHEAD > out->available_bytes)
        return exitProcess(state, DENSITY_LION_ENCODE_PROCESS_CHECK_SIGNATURE_STATE, DENSITY_KERNEL_ENCODE_STATE_STALL_ON_OUTPUT);

    // Try to read a complete chunk unit
    read_chunk:
    pointerOutBefore = out->pointer;
    if (!(readMemoryLocation = density_memory_teleport_read(in, DENSITY_LION_ENCODE_PROCESS_UNIT_SIZE)))
        return exitProcess(state, DENSITY_LION_ENCODE_PROCESS_READ_CHUNK, DENSITY_KERNEL_ENCODE_STATE_STALL_ON_INPUT);

    // Chunk was read properly, process
    density_lion_encode_process_unit(&chunk, readMemoryLocation, out, &hash, state);
    readMemoryLocation->available_bytes -= DENSITY_LION_ENCODE_PROCESS_UNIT_SIZE;
    out->available_bytes -= (out->pointer - pointerOutBefore);

    // New loop
    goto check_signature_state;
}

DENSITY_FORCE_INLINE DENSITY_KERNEL_ENCODE_STATE density_lion_encode_finish(density_memory_teleport *restrict in, density_memory_location *restrict out, density_lion_encode_state *restrict state) {
    DENSITY_KERNEL_ENCODE_STATE returnState;
    uint32_t hash;
    uint64_t chunk;
    density_memory_location *readMemoryLocation;
    density_byte *pointerOutBefore;

    // Dispatch
    switch (state->process) {
        case DENSITY_LION_ENCODE_PROCESS_PREPARE_NEW_BLOCK:
            goto prepare_new_block;
        case DENSITY_LION_ENCODE_PROCESS_READ_CHUNK:
            goto read_chunk;
        case DENSITY_LION_ENCODE_PROCESS_CHECK_SIGNATURE_STATE:
            goto check_signature_state;
        default:
            return DENSITY_KERNEL_ENCODE_STATE_ERROR;
    }

    // Prepare new block
    prepare_new_block:
    if ((returnState = density_lion_encode_prepare_new_block(out, state)))
        return exitProcess(state, DENSITY_LION_ENCODE_PROCESS_PREPARE_NEW_BLOCK, returnState);

    check_signature_state:
    if (DENSITY_LION_ENCODE_MINIMUM_OUTPUT_LOOKAHEAD > out->available_bytes)
        return exitProcess(state, DENSITY_LION_ENCODE_PROCESS_CHECK_SIGNATURE_STATE, DENSITY_KERNEL_ENCODE_STATE_STALL_ON_OUTPUT);

    // Try to read a complete chunk unit
    read_chunk:
    pointerOutBefore = out->pointer;
    if (!(readMemoryLocation = density_memory_teleport_read(in, DENSITY_LION_ENCODE_PROCESS_UNIT_SIZE)))
        goto step_by_step;

    // Chunk was read properly, process
    density_lion_encode_process_unit(&chunk, readMemoryLocation, out, &hash, state);
    readMemoryLocation->available_bytes -= DENSITY_LION_ENCODE_PROCESS_UNIT_SIZE;
    goto exit;

    // Read step by step
    step_by_step:
    while (state->signatureData.shift != density_bitsizeof(density_kernel_signature) && (readMemoryLocation = density_memory_teleport_read(in, sizeof(uint32_t)))) {
        density_lion_encode_kernel(out, &hash, *(uint32_t *) (readMemoryLocation->pointer), state);
        readMemoryLocation->pointer += sizeof(uint32_t);
        readMemoryLocation->available_bytes -= sizeof(uint32_t);
    }
    exit:
    out->available_bytes -= (out->pointer - pointerOutBefore);

    // New loop
    if (density_memory_teleport_available(in) >= sizeof(uint32_t))
        goto check_signature_state;

    // Marker for decode loop exit
    const density_lion_entropy_code code = density_lion_encode_fetch_form_rank_for_use(state, DENSITY_LION_FORM_CHUNK_DICTIONARY_A);
    DENSITY_KERNEL_ENCODE_PUSH_TO_SIGNATURE(out, &state->signatureData, code.value, code.bitLength);
    DENSITY_KERNEL_ENCODE_PUSH_REGISTER_SIGNATURE_TO_MEMORY(&state->signatureData);

    // Copy the remaining bytes
    density_memory_teleport_copy_remaining(in, out);

    //done_encoding(out, state);

    return DENSITY_KERNEL_ENCODE_STATE_READY;
}