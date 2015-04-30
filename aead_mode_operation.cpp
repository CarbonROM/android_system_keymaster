/*
 * Copyright 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>

#include <openssl/aes.h>
#include <openssl/rand.h>

#include <keymaster/authorization_set.h>
#include <keymaster/logger.h>

#include "aead_mode_operation.h"
#include "openssl_err.h"

namespace keymaster {

keymaster_error_t AeadModeOperation::Begin(const AuthorizationSet& input_params,
                                           AuthorizationSet* output_params) {
    keymaster_error_t error = Initialize(key_, key_size_, nonce_length_, tag_length_);
    if (error != KM_ERROR_OK)
        return error;

    buffer_end_ = 0;
    buffer_.reset(new uint8_t[processing_unit_]);
    if (!buffer_.get())
        return KM_ERROR_MEMORY_ALLOCATION_FAILED;

    return HandleNonce(input_params, output_params);
}

inline size_t min(size_t a, size_t b) {
    if (a < b)
        return a;
    return b;
}

keymaster_error_t AeadModeOperation::Update(const AuthorizationSet& additional_params,
                                            const Buffer& input, Buffer* output,
                                            size_t* input_consumed) {
    // Make an effort to reserve enough output space.  The output buffer will be extended if needed,
    // but this reduces reallocations.
    if (!output->reserve(EstimateOutputSize(input, output)))
        return KM_ERROR_MEMORY_ALLOCATION_FAILED;

    keymaster_error_t error = KM_ERROR_OK;
    *input_consumed = 0;

    keymaster_blob_t associated_data = {0, 0};
    additional_params.GetTagValue(TAG_ASSOCIATED_DATA, &associated_data);

    const uint8_t* plaintext = input.peek_read();
    const uint8_t* plaintext_end = plaintext + input.available_read();
    while (plaintext < plaintext_end && error == KM_ERROR_OK) {
        if (buffered_data_length() == processing_unit_) {
            error = ProcessChunk(associated_data, output);
            ClearBuffer();
            IncrementNonce();
        }
        plaintext = AppendToBuffer(plaintext, plaintext_end - plaintext);
        *input_consumed = plaintext - input.peek_read();
    }
    return error;
}

keymaster_error_t AeadModeOperation::Finish(const AuthorizationSet& additional_params,
                                            const Buffer& /* signature */, Buffer* output) {
    keymaster_blob_t associated_data = {0, 0};
    additional_params.GetTagValue(TAG_ASSOCIATED_DATA, &associated_data);
    return ProcessChunk(associated_data, output);
}

keymaster_error_t AeadModeOperation::ProcessChunk(const keymaster_blob_t& additional_data,
                                                  Buffer* output) {
    keymaster_error_t error = KM_ERROR_OK;
    if (purpose() == KM_PURPOSE_DECRYPT) {
        if (buffered_data_length() < tag_length_)
            return KM_ERROR_INVALID_INPUT_LENGTH;
        ExtractTagFromBuffer();
        LOG_D("AeadMode decrypting %d", buffered_data_length());
        if (!output->reserve(output->available_read() + buffered_data_length()))
            error = KM_ERROR_MEMORY_ALLOCATION_FAILED;
        else
            error = DecryptChunk(nonce_, nonce_length_, tag_, tag_length_, additional_data,
                                 buffer_.get(), buffered_data_length(), output);
    } else {
        if (!output->reserve(output->available_read() + buffered_data_length() + tag_length_))
            error = KM_ERROR_MEMORY_ALLOCATION_FAILED;
        else
            error = EncryptChunk(nonce_, nonce_length_, tag_length_, additional_data, buffer_.get(),
                                 buffered_data_length(), output);
    }
    return error;
}

size_t AeadModeOperation::EstimateOutputSize(const Buffer& input, Buffer* output) {
    switch (purpose()) {
    case KM_PURPOSE_ENCRYPT: {
        size_t chunk_length = processing_unit_;
        size_t chunk_count = (input.available_read() + chunk_length - 1) / chunk_length;
        return output->available_read() + nonce_length_ +
               chunk_count * (chunk_length + tag_length_);
    }
    case KM_PURPOSE_DECRYPT: {
        size_t chunk_length = processing_unit_ - tag_length_;
        size_t chunk_count =
            (input.available_read() - nonce_length_ + processing_unit_ - 1) / processing_unit_;
        return output->available_read() + chunk_length * chunk_count;
    }
    default:
        LOG_E("Encountered invalid purpose %d", purpose());
        return 0;
    }
}

keymaster_error_t AeadModeOperation::HandleNonce(const AuthorizationSet& input_params,
                                                 AuthorizationSet* output_params) {
    if (!output_params)
        return KM_ERROR_OUTPUT_PARAMETER_NULL;

    switch (purpose()) {
    case KM_PURPOSE_ENCRYPT: {
        keymaster_error_t error;
        if (caller_nonce_)
            error = ExtractNonce(input_params);
        else {
            if (input_params.find(TAG_NONCE) != -1)
                return KM_ERROR_CALLER_NONCE_PROHIBITED;
            error = GenerateNonce();
        }

        if (error != KM_ERROR_OK)
            return error;

        output_params->push_back(TAG_NONCE, nonce_, nonce_length_);
        if (output_params->is_valid() != AuthorizationSet::OK)
            return KM_ERROR_MEMORY_ALLOCATION_FAILED;
        break;
    }
    case KM_PURPOSE_DECRYPT:
        return ExtractNonce(input_params);

    default:
        return KM_ERROR_UNSUPPORTED_PURPOSE;
    }
    return KM_ERROR_OK;
}

keymaster_error_t AeadModeOperation::GenerateNonce() {
    if (RAND_bytes(nonce_, nonce_length_))
        return KM_ERROR_OK;
    LOG_S("Failed to generate %d-byte nonce", nonce_length_);
    return TranslateLastOpenSslError();
}

keymaster_error_t AeadModeOperation::ExtractNonce(const AuthorizationSet& input_params) {
    keymaster_blob_t nonce_blob;
    if (!input_params.GetTagValue(TAG_NONCE, &nonce_blob))
        return KM_ERROR_MISSING_NONCE;

    if (nonce_blob.data_length != nonce_length_) {
        LOG_E("Expected %d-byte nonce, got %d bytes", nonce_length_, nonce_blob.data_length);
        return KM_ERROR_INVALID_NONCE;
    }

    memcpy(nonce_, nonce_blob.data, nonce_length_);
    return KM_ERROR_OK;
}

void AeadModeOperation::IncrementNonce() {
    for (int i = nonce_length_ - 1; i > 0; --i)
        if (++nonce_[i])
            break;
}

const uint8_t* AeadModeOperation::AppendToBuffer(const uint8_t* data, size_t data_length) {
    // Only take as much data as we can fit.
    if (data_length > buffer_free_space())
        data_length = buffer_free_space();
    memcpy(buffer_.get() + buffer_end_, data, data_length);
    buffer_end_ += data_length;
    return data + data_length;
}

void AeadModeOperation::ExtractTagFromBuffer() {
    assert(buffered_data_length() >= tag_length_);
    memcpy(tag_, buffer_.get() + buffer_end_ - tag_length_, tag_length_);
    buffer_end_ -= tag_length_;
}

}  // namespace keymaster
