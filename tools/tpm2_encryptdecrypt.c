//**********************************************************************;
// Copyright (c) 2015-2018, Intel Corporation
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// 3. Neither the name of Intel Corporation nor the names of its contributors
// may be used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//**********************************************************************;

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <limits.h>
#include <ctype.h>

#include <tss2/tss2_esys.h>

#include "files.h"
#include "log.h"
#include "tpm2_alg_util.h"
#include "tpm2_auth_util.h"
#include "tpm2_options.h"
#include "tpm2_session.h"
#include "tpm2_tool.h"
#include "tpm2_util.h"

typedef struct tpm_encrypt_decrypt_ctx tpm_encrypt_decrypt_ctx;
struct tpm_encrypt_decrypt_ctx {
    struct {
        TPMS_AUTH_COMMAND session_data;
        tpm2_session *session;
    } auth;
    TPMI_YES_NO is_decrypt;
    TPM2B_MAX_BUFFER data;
    char *input_path;
    char *out_file_path;
    const char *context_arg;
    tpm2_loaded_object key_context_object;
    TPMI_ALG_SYM_MODE mode;
    struct {
        char *in;
        char *out;
    } iv;
    struct {
        UINT8 p : 1;
        UINT8 D : 1;
        UINT8 i : 1;
        UINT8 X : 1;
    } flags;
    char *key_auth_str;
};

static tpm_encrypt_decrypt_ctx ctx = {
    .mode = TPM2_ALG_NULL,
    .auth = { .session_data = TPMS_AUTH_COMMAND_INIT(TPM2_RS_PW) },
};

static bool readpub(ESYS_CONTEXT *ectx, TPM2B_PUBLIC **public) {

    TSS2_RC rval = Esys_ReadPublic(ectx, ctx.key_context_object.tr_handle,
                      ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                      public, NULL, NULL);
    if (rval != TPM2_RC_SUCCESS) {
        LOG_PERR(Esys_ReadPublic, rval);
        return false;
    }

    return true;
}

static bool encrypt_decrypt(ESYS_CONTEXT *ectx, TPM2B_IV *iv_in) {

    TPM2B_MAX_BUFFER *out_data;
    TPM2B_IV *iv_out;

    /*
     * try EncryptDecrypt2 first, and if the command is not supported by the TPM
     * fall back to EncryptDecrypt. Keep track of which version you ran,
     * for error reporting.
     */
    unsigned version = 2;

    ESYS_TR shandle1 = tpm2_auth_util_get_shandle(ectx,
                            ctx.key_context_object.tr_handle,
                            &ctx.auth.session_data, ctx.auth.session);
    if (shandle1 == ESYS_TR_NONE) {
        LOG_ERR("Failed to get shandle");
        return false;
    }

    TSS2_RC rval = Esys_EncryptDecrypt2(ectx, ctx.key_context_object.tr_handle,
                      shandle1, ESYS_TR_NONE, ESYS_TR_NONE,
                      &ctx.data, ctx.is_decrypt, ctx.mode, iv_in,
                      &out_data, &iv_out);
    if (tpm2_error_get(rval) == TPM2_RC_COMMAND_CODE) {
        version = 1;
        rval = Esys_EncryptDecrypt(ectx, ctx.key_context_object.tr_handle,
                  shandle1, ESYS_TR_NONE, ESYS_TR_NONE,
                  ctx.is_decrypt, ctx.mode, iv_in, &ctx.data,
                  &out_data, &iv_out);
    }
    if (rval != TPM2_RC_SUCCESS) {
        if (version == 2) {
            LOG_PERR(Esys_EncryptDecrypt2, rval);
        } else {
            LOG_PERR(Esys_EncryptDecrypt, rval);
        }
        return false;
    }

    bool result = files_save_bytes_to_file(ctx.out_file_path, out_data->buffer,
            out_data->size);

    if (result) {
        result = ctx.iv.out ? files_save_bytes_to_file(ctx.iv.out,
                                iv_out->buffer,
                                iv_out->size) : true;
    }

    free(out_data);
    free(iv_out);

    return result;
}

static void parse_iv(char *value) {

    ctx.iv.in = value;

    char *split = strchr(value, ':');
    if (split) {
        *split = '\0';
        split++;
        if (split) {
            ctx.iv.out = split;
        }
    }
}

static bool on_option(char key, char *value) {

    switch (key) {
    case 'c':
        ctx.context_arg = value;
        break;
    case 'p':
        ctx.flags.p = 1;
        ctx.key_auth_str = value;
        break;
    case 'D':
        ctx.is_decrypt = 1;
        break;
    case 'i':
        ctx.input_path = value;
        ctx.flags.i = 1;
        break;
    case 'o':
        ctx.out_file_path = value;
        break;
    case 'G':
        ctx.mode = tpm2_alg_util_strtoalg(value, tpm2_alg_util_flags_mode);
        if (ctx.mode == TPM2_ALG_ERROR) {
            LOG_ERR("Invalid mode, got: %s", value);
            return false;
        }
        break;
    case 't':
        parse_iv(value);
        break;
    }

    return true;
}

bool tpm2_tool_onstart(tpm2_options **opts) {

    const struct option topts[] = {
        { "auth-key",             required_argument, NULL, 'p' },
        { "decrypt",              no_argument,       NULL, 'D' },
        { "in-file",              required_argument, NULL, 'i' },
        { "iv",                   required_argument, NULL, 't' },
        { "mode",                 required_argument, NULL, 'G' },
        { "out-file",             required_argument, NULL, 'o' },
        { "key-context",          required_argument, NULL, 'c' },
    };

    *opts = tpm2_options_new("p:Di:o:c:i:G:t:", ARRAY_LEN(topts), topts, on_option,
                             NULL, 0);

    return *opts != NULL;
}

int tpm2_tool_onrun(ESYS_CONTEXT *ectx, tpm2_option_flags flags) {

    UNUSED(flags);

    bool result;
    int rc = 1;

    if (!ctx.context_arg) {
        LOG_ERR("Expected a context file or handle, got none.");
        return -1;
    }

    ctx.data.size = sizeof(ctx.data.buffer);
    result = files_load_bytes_from_buffer_or_file_or_stdin(NULL,ctx.input_path,
        &ctx.data.size, ctx.data.buffer);
    if (!result) {
        return false;
    }

    result = tpm2_util_object_load(ectx, ctx.context_arg,
                                &ctx.key_context_object);
    if (!result) {
        goto out;
    }

    if (ctx.flags.p) {
        result = tpm2_auth_util_from_optarg(ectx, ctx.key_auth_str,
                &ctx.auth.session_data, &ctx.auth.session);
        if (!result) {
            LOG_ERR("Invalid object key authorization, got\"%s\"",
                ctx.key_auth_str);
            goto out;
        }
    }

    /*
     * Sym objects can have a NULL mode, which means the caller can and must determine mode.
     * Thus if the caller doesn't specify an algorithm, and the object has a default mode, choose it,
     * else choose CFB.
     * If the caller specifies an invalid mode, just pass it to the TPM and let it error out.
     */
    if (ctx.mode == TPM2_ALG_NULL) {

        TPM2B_PUBLIC *public;
        result = readpub(ectx, &public);
        if (!result) {
            goto out;
        }

        TPMI_ALG_SYM_MODE objmode = public->publicArea.parameters.symDetail.sym.mode.sym;
        if (objmode == TPM2_ALG_NULL) {
            ctx.mode = TPM2_ALG_CFB;
        } else {
            ctx.mode = objmode;
        }

        free(public);
    }

    TPM2B_IV iv = { .size = sizeof(iv.buffer), .buffer = { 0 } };
    if (ctx.iv.in) {
        unsigned long file_size;
        result = files_get_file_size_path(ctx.iv.in, &file_size);
        if (!result) {
            goto out;
        }

        if (file_size != iv.size) {
            LOG_ERR("Iv should be 16 bytes, got %lu", file_size);
            goto out;
        }

        result = files_load_bytes_from_path(ctx.iv.in, iv.buffer, &iv.size);
        if (!result) {
            goto out;
        }

    }

    if (!ctx.iv.in) {
        LOG_WARN("Using a weak IV, try specifying an IV");
    }

    result = encrypt_decrypt(ectx, &iv);
    if (!result) {
        goto out;
    }

    rc = 0;
out:
    result = tpm2_session_save(ectx, ctx.auth.session, NULL);
    if (!result) {
        rc = 1;
    }

    return rc;
}

void tpm2_onexit(void) {

    tpm2_session_free(&ctx.auth.session);
}
