#ifndef VW_MODEL_CATALOG_H_
#define VW_MODEL_CATALOG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Catalog entry mirrors models/manifest.json; keep both files synchronized when
// adding models. Base URL is https://huggingface.co/ggerganov/whisper.cpp/resolve/main/.
typedef struct vw_model_catalog_entry {
  const char* id;
  const char* filename;
  const char* url;
  const char* sha256_hex;
  uint64_t bytes;
  bool multilingual;
} vw_model_catalog_entry_t;

typedef vw_model_catalog_entry_t vw_model_catalog_entry_t_alias;

static const vw_model_catalog_entry_t kVWModelCatalogEntries[] = {
    {"tiny.en", "ggml-tiny.en.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin",
     "921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f", 77704715ULL, false},
    {"tiny", "ggml-tiny.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin",
     "be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21", 77691713ULL, true},
    {"base.en", "ggml-base.en.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin",
     "a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002", 147964211ULL, false},
    {"base", "ggml-base.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin",
     "60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe", 147951465ULL, true},
    {"small", "ggml-small.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin",
     "1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b", 487601967ULL, true},
    {"medium", "ggml-medium.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.bin",
     "6c14d5adee5f86394037b4e4e8b59f1673b6cee10e3cf0b11bbdbee79c156208", 1533763059ULL, true},
    {"large", "ggml-large-v3.bin", "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3.bin",
     "64d182b440b98d5203c4f9bd541544d84c605196c4f7b845dfa11fb23594d1e2", 3095033483ULL, true},
    {"large-v3-turbo", "ggml-large-v3-turbo.bin",
     "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo.bin",
     "1fc70f774d38eb169993ac391eea357ef47c88757ef72ee5943879b7e8e2bc69", 1624555275ULL, true},
};

static const size_t kVWModelCatalogCount = sizeof(kVWModelCatalogEntries) / sizeof(kVWModelCatalogEntries[0]);

// Returns the number of committed catalog entries; mirrors JSON manifest length
// and is stable across translation units without requiring separate linkage.
static inline size_t vw_model_catalog_count(void) { return kVWModelCatalogCount; }

// Finds a catalog entry by short identifier (e.g. "small") using case-sensitive
// compare; returns pointer to static entry or NULL when identifier unknown.
static inline const vw_model_catalog_entry_t* vw_model_catalog_find(const char* id) {
  if (!id) return NULL;
  for (size_t i = 0; i < kVWModelCatalogCount; i++) {
    if (strcmp(kVWModelCatalogEntries[i].id, id) == 0) return &kVWModelCatalogEntries[i];
  }
  return NULL;
}

#ifdef __cplusplus
}
#endif

#endif  // VW_MODEL_CATALOG_H_
