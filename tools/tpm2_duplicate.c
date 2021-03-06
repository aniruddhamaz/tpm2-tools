//**********************************************************************;
// Copyright (c) 2017-2018, Intel Corporation
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
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <tss2/tss2_esys.h>

#include "log.h"
#include "files.h"
#include "tpm2_alg_util.h"
#include "tpm2_auth_util.h"
#include "tpm2_options.h"

typedef struct tpm_duplicate_ctx tpm_duplicate_ctx;
struct tpm_duplicate_ctx {
    struct {
        TPMS_AUTH_COMMAND session_data;
        tpm2_session *session;
    } auth;
    char *duplicate_key_public_file;
    char *duplicate_key_private_file;

    TPMI_ALG_PUBLIC key_type;
    char *sym_key_in;
    char *sym_key_out;

    char *enc_seed_out;

    const char *new_parent_object_arg;
    tpm2_loaded_object new_parent_object_context;

    char *object_auth_str;
    const char *object_arg;
    tpm2_loaded_object object_context;

    struct {
        UINT16 c : 1;
        UINT16 C : 1;
        UINT16 g : 1;
        UINT16 i : 1;
        UINT16 o : 1;
        UINT16 p : 1;
        UINT16 r : 1;
        UINT16 s : 1;
    } flags;

};

static tpm_duplicate_ctx ctx = {
    .key_type = TPM2_ALG_ERROR,
    .auth.session_data = TPMS_AUTH_COMMAND_INIT(TPM2_RS_PW),
};

static bool do_duplicate(ESYS_CONTEXT *ectx,
        TPM2B_DATA *in_key,
        TPMT_SYM_DEF_OBJECT *sym_alg,
        TPM2B_DATA **out_key,
        TPM2B_PRIVATE **duplicate,
        TPM2B_ENCRYPTED_SECRET **encrypted_seed) {

    ESYS_TR shandle1 = tpm2_auth_util_get_shandle(ectx,
                            ctx.object_context.tr_handle,
                            &ctx.auth.session_data, ctx.auth.session);
    if (shandle1 == ESYS_TR_NONE) {
        LOG_ERR("Failed to get shandle");
        tpm2_session_free(&ctx.auth.session);
        return false;
    }

    TSS2_RC rval = Esys_Duplicate(ectx,
                        ctx.object_context.tr_handle, ctx.new_parent_object_context.tr_handle,
                        shandle1, ESYS_TR_NONE, ESYS_TR_NONE,
                        in_key, sym_alg, out_key, duplicate, encrypted_seed);
    if (rval != TPM2_RC_SUCCESS) {
        LOG_PERR(Esys_Duplicate, rval);
        tpm2_session_free(&ctx.auth.session);
        return false;
    }

    return true;
}

static bool on_option(char key, char *value) {

    switch(key) {
    case 'p':
        ctx.object_auth_str = value;
        ctx.flags.p = 1;
        break;
    case 'g':
        ctx.key_type = tpm2_alg_util_from_optarg(value,
                tpm2_alg_util_flags_symmetric
                |tpm2_alg_util_flags_misc);
        if (ctx.key_type != TPM2_ALG_ERROR) {
            ctx.flags.g = 1;
        }
        break;
    case 'i':
        ctx.sym_key_in = value;
        ctx.flags.i = 1;
        break;
    case 'o':
        ctx.sym_key_out = value;
        ctx.flags.o = 1;
        break;
    case 'C':
        ctx.new_parent_object_arg = value;
        ctx.flags.C = 1;
        break;
    case 'c':
        ctx.object_arg = value;
        ctx.flags.c = 1;
        break;
    case 'r':
        ctx.duplicate_key_private_file = value;
        ctx.flags.r = 1;
        break;
    case 's':
        ctx.enc_seed_out = value;
        ctx.flags.s = 1;
        break;
    default:
        LOG_ERR("Invalid option");
        return false;
    }

    return true;
}

bool tpm2_tool_onstart(tpm2_options **opts) {

    const struct option topts[] = {
      { "auth-key",              required_argument, NULL, 'p'},
      { "inner-wrapper-alg",     required_argument, NULL, 'g'},
      { "duplicate-key-private", required_argument, NULL, 'r'},
      { "input-key-file",        required_argument, NULL, 'i'},
      { "output-key-file",       required_argument, NULL, 'o'},
      { "output-enc-seed-file",  required_argument, NULL, 's'},
      { "parent-key",            required_argument, NULL, 'C'},
      { "context",               required_argument, NULL, 'c'},
    };

    *opts = tpm2_options_new("p:g:i:C:o:s:r:c:", ARRAY_LEN(topts), topts, on_option,
                             NULL, 0);

    return *opts != NULL;
}

/**
 * Check all options and report as many errors as possible via LOG_ERR.
 * @return
 *  true on success, false on failure.
 */
static bool check_options(void) {

    bool result = true;

    /* Check for NULL alg & (keyin | keyout) */
    if (ctx.flags.g == 0) {
        LOG_ERR("Expected key type to be specified via \"-G\","
                " missing option.");
        result = false;
    }

    if (ctx.key_type != TPM2_ALG_NULL) {
        if((ctx.flags.i == 0) && (ctx.flags.o == 0)) {
            LOG_ERR("Expected in or out encryption key file \"-k/K\","
                    " missing option.");
            result = false;
        }
        if (ctx.flags.i && ctx.flags.o) {
            LOG_ERR("Expected either in or out encryption key file \"-k/K\","
                    " conflicting options.");
            result = false;
        }
    } else {
        if (ctx.flags.i || ctx.flags.o) {
            LOG_ERR("Expected neither in nor out encryption key file \"-k/K\","
                    " conflicting options.");
            result = false;
        }
    }

    if (ctx.flags.C == 0) {
        LOG_ERR("Expected new parent object to be specified via \"-C\","
                " missing option.");
        result = false;
    }

    if (ctx.flags.c == 0) {
        LOG_ERR("Expected object to be specified via \"-c\","
                " missing option.");
        result = false;
    }

    if (ctx.flags.s == 0) {
        LOG_ERR("Expected encrypted seed out filename to be specified via \"-S\","
                " missing option.");
        result = false;
    }

    if (ctx.flags.r == 0) {
        LOG_ERR("Expected private key out filename to be specified via \"-r\","
                " missing option.");
        result = false;
    }

    return result;
}

static bool set_key_algorithm(TPMI_ALG_PUBLIC alg, TPMT_SYM_DEF_OBJECT * obj) {
    bool result = true;
    switch (alg) {
    case TPM2_ALG_AES :
        obj->algorithm = TPM2_ALG_AES;
        obj->keyBits.aes = 128;
        obj->mode.aes = TPM2_ALG_CFB;
        break;
    case TPM2_ALG_NULL :
        obj->algorithm = TPM2_ALG_NULL;
        break;
    default:
        LOG_ERR("The algorithm type input(0x%x) is not supported!", alg);
        result = false;
        break;
    }
    return result;
}

int tpm2_tool_onrun(ESYS_CONTEXT *ectx, tpm2_option_flags flags) {
    UNUSED(flags);

    int rc = 1;
    bool result;
    TPMT_SYM_DEF_OBJECT sym_alg;
    TPM2B_DATA in_key;
    TPM2B_DATA* out_key = NULL;
    TPM2B_PRIVATE* duplicate;
    TPM2B_ENCRYPTED_SECRET* outSymSeed;

    result = check_options();
    if (!result) {
        goto out;
    }

    result = tpm2_util_object_load(ectx, ctx.object_arg,
		    &ctx.object_context);
    if(!result) {
        goto out;
    }

    result = tpm2_util_object_load(ectx, ctx.new_parent_object_arg,
		    &ctx.new_parent_object_context);
    if(!result) {
        goto out;
    }

    if (ctx.flags.p) {
        result = tpm2_auth_util_from_optarg(ectx, ctx.object_auth_str,
            &ctx.auth.session_data, &ctx.auth.session);
        if (!result) {
            LOG_ERR("Invalid authorization, got\"%s\"", ctx.object_auth_str);
            goto out;
        }
    }

    result = set_key_algorithm(ctx.key_type, &sym_alg);
    if(!result) {
        goto out;
    }

    if(ctx.flags.i) {
        in_key.size = 16;
        result = files_load_bytes_from_path(ctx.sym_key_in, in_key.buffer, &in_key.size);
        if(!result) {
            goto out;
        }
        if(in_key.size != 16) {
            LOG_ERR("Invalid AES key size, got %u bytes, expected 16", in_key.size);
            goto out;
        }
    }

    result = do_duplicate(ectx,
        ctx.flags.i ? &in_key : NULL,
        &sym_alg,
        ctx.flags.o ? &out_key : NULL,
        &duplicate,
        &outSymSeed);
    if (!result) {
        goto out;
    }

    result = tpm2_session_save(ectx, ctx.auth.session, NULL);
    if (!result) {
        goto free_out;
    }

    /* Maybe a false positive from scan-build but we'll check out_key anyway */
    if (ctx.flags.o) {
        if(out_key == NULL) {
            LOG_ERR("No encryption key from TPM ");
            goto free_out;
        }
        result = files_save_bytes_to_file(ctx.sym_key_out,
                    out_key->buffer, out_key->size);
        if (!result) {
            LOG_ERR("Failed to save encryption key out into file \"%s\"",
                    ctx.sym_key_out);
            goto free_out;
        }
    }

    result = files_save_encrypted_seed(outSymSeed, ctx.enc_seed_out);
    if (!result) {
        LOG_ERR("Failed to save encryption seed into file \"%s\"",
                ctx.enc_seed_out);
        goto free_out;
    }

    result = files_save_private(duplicate, ctx.duplicate_key_private_file);
    if (!result) {
        LOG_ERR("Failed to save private key into file \"%s\"",
                ctx.duplicate_key_private_file);
        goto free_out;
    }

    rc = 0;

free_out:
    free(out_key);
    free(outSymSeed);
    free(duplicate);
out:
    return rc;
}

void tpm2_onexit(void) {
    tpm2_session_free(&ctx.auth.session);
}
