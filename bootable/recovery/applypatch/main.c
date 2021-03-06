/*
 * Copyright (C) 2009 The Android Open Source Project
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applypatch.h"
#include "edify/expr.h"
#include "mincrypt/sha.h"

int CheckMode(int argc, char** argv) {
    if (argc < 3) {
        return 2;
    }
    return applypatch_check(argv[2], argc-3, argv+3);
}

int SpaceMode(int argc, char** argv) {
    if (argc != 3) {
        return 2;
    }
    char* endptr;
    size_t bytes = strtol(argv[2], &endptr, 10);
    if (bytes == 0 && endptr == argv[2]) {
        printf("can't parse \"%s\" as byte count\n\n", argv[2]);
        return 1;
    }
    return CacheSizeCheck(bytes);
}

int TeeUpdateMode(int argc, char** argv) {
    if (false) {
        return 2;
    }
	
    return TeeUpdate(argv[2],argv[3]);
}

// Parse arguments (which should be of the form "<sha1>" or
// "<sha1>:<filename>" into the new parallel arrays *sha1s and
// *patches (loading file contents into the patches).  Returns 0 on
// success.
static int ParsePatchArgs(int argc, char** argv,
                          char*** sha1s, Value*** patches, int* num_patches) {
    *num_patches = argc;
    *sha1s = malloc(*num_patches * sizeof(char*));
    *patches = malloc(*num_patches * sizeof(Value*));
    memset(*patches, 0, *num_patches * sizeof(Value*));

    uint8_t digest[SHA_DIGEST_SIZE];

    int i;
    for (i = 0; i < *num_patches; ++i) {
        char* colon = strchr(argv[i], ':');
        if (colon != NULL) {
            *colon = '\0';
            ++colon;
        }

        if (ParseSha1(argv[i], digest) != 0) {
            printf("failed to parse sha1 \"%s\"\n", argv[i]);
            return -1;
        }

        (*sha1s)[i] = argv[i];
        if (colon == NULL) {
            (*patches)[i] = NULL;
        } else {
            FileContents fc;
            if (LoadFileContents(colon, &fc, RETOUCH_DONT_MASK) != 0) {
                goto abort;
            }
            (*patches)[i] = malloc(sizeof(Value));
            (*patches)[i]->type = VAL_BLOB;
            (*patches)[i]->size = fc.size;
            (*patches)[i]->data = (char*)fc.data;
        }
    }

    return 0;

  abort:
    for (i = 0; i < *num_patches; ++i) {
        Value* p = (*patches)[i];
        if (p != NULL) {
            free(p->data);
            free(p);
        }
    }
    free(*sha1s);
    free(*patches);
    return -1;
}

int PatchMode(int argc, char** argv) {
    Value* bonus = NULL;
    if (argc >= 3 && strcmp(argv[1], "-b") == 0) {
        FileContents fc;
        if (LoadFileContents(argv[2], &fc, RETOUCH_DONT_MASK) != 0) {
            printf("failed to load bonus file %s\n", argv[2]);
            return 1;
        }
        bonus = malloc(sizeof(Value));
        bonus->type = VAL_BLOB;
        bonus->size = fc.size;
        bonus->data = (char*)fc.data;
        argc -= 2;
        argv += 2;
    }

    if (argc < 6) {
        return 2;
    }

    char* endptr;
    size_t target_size = strtol(argv[4], &endptr, 10);
    if (target_size == 0 && endptr == argv[4]) {
        printf("can't parse \"%s\" as byte count\n\n", argv[4]);
        return 1;
    }

    char** sha1s;
    Value** patches;
    int num_patches;
    if (ParsePatchArgs(argc-5, argv+5, &sha1s, &patches, &num_patches) != 0) {
        printf("failed to parse patch args\n");
        return 1;
    }

    int result = applypatch(argv[1], argv[2], argv[3], target_size,
                            num_patches, sha1s, patches, bonus);

    int i;
    for (i = 0; i < num_patches; ++i) {
        Value* p = patches[i];
        if (p != NULL) {
            free(p->data);
            free(p);
        }
    }
    if (bonus) {
        free(bonus->data);
        free(bonus);
    }
    free(sha1s);
    free(patches);

    return result;
}

// This program applies binary patches to files in a way that is safe
// (the original file is not touched until we have the desired
// replacement for it) and idempotent (it's okay to run this program
// multiple times).
//
// - if the sha1 hash of <tgt-file> is <tgt-sha1>, does nothing and exits
//   successfully.
//
// - otherwise, if the sha1 hash of <src-file> is <src-sha1>, applies the
//   bsdiff <patch> to <src-file> to produce a new file (the type of patch
//   is automatically detected from the file header).  If that new
//   file has sha1 hash <tgt-sha1>, moves it to replace <tgt-file>, and
//   exits successfully.  Note that if <src-file> and <tgt-file> are
//   not the same, <src-file> is NOT deleted on success.  <tgt-file>
//   may be the string "-" to mean "the same as src-file".
//
// - otherwise, or if any error is encountered, exits with non-zero
//   status.
//
// <src-file> (or <file> in check mode) may refer to an MTD partition
// to read the source data.  See the comments for the
// LoadMTDContents() function above for the format of such a filename.

int main(int argc, char** argv) {
    if (argc < 2) {
      usage:
        printf(
            "usage: %s [-b <bonus-file>] <src-file> <tgt-file> <tgt-sha1> <tgt-size> "
            "[<src-sha1>:<patch> ...]\n"
            "   or  %s -c <file> [<sha1> ...]\n"
            "   or  %s -s <bytes>\n"
            "   or  %s -l\n"
            "\n"
            "Filenames may be of the form\n"
            "  MTD:<partition>:<len_1>:<sha1_1>:<len_2>:<sha1_2>:...\n"
            "to specify reading from or writing to an MTD partition.\n\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }

    int result;

    if (strncmp(argv[1], "-l", 3) == 0) {
        result = ShowLicenses();
    } else if (strncmp(argv[1], "-c", 3) == 0) {
        result = CheckMode(argc, argv);
    } else if (strncmp(argv[1], "-s", 3) == 0) {
        result = SpaceMode(argc, argv);
    } else if (strncmp(argv[1], "-t", 3) == 0) {
        result = TeeUpdateMode(argc, argv);
    } else {
        result = PatchMode(argc, argv);
    }

    if (result == 2) {
        goto usage;
    }
    return result;
}
