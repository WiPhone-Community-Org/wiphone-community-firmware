// GENERATED ONCE by tools/gen_golden_positions.cpp from the PRE-KOSync parser
// (git 1de15a6, 0.9.78). 🛑 Do not regenerate from a later revision: this is the
// "before" that test_kosync.cpp proves the current parser still matches.
#pragma once
#include <stdint.h>
typedef struct { int spine; uint32_t offset; double fraction; } GpFrac;
typedef struct { double fraction; int spine; uint32_t offset; } GpLoc;
typedef struct {
  const char* file; int status; int nSpine; const char* const* names;
  const uint32_t* chapLen; const char* const* ids; int nIds;
  const GpFrac* fr; int nFr; const GpLoc* loc; int nLoc;
  int savedSpine; uint32_t savedOffset; double savedFraction;
} GpBook;
static const char* const GP_B0_NAMES[] = { "only.xhtml", "" };
static const uint32_t GP_B0_LEN[] = { 31u, 0u };
static const char* const GP_B0_IDS[] = { "ta:no-spine-at-all", "fp:8ba8cf95cb4411da", "" };
static const GpFrac GP_B0_FR[] = {
  { 0, 0u, 0 },
  { 0, 10u, 0.32258064516129031 },
  { 0, 15u, 0.4838709677419355 },
  { 0, 30u, 0.967741935483871 },
  { 0, 31u, 1 },
  { 0, 38u, 1 },
  { 0, 0u, 0 }
};
static const GpLoc GP_B0_LOC[] = {
  { 0, 0, 0u },
  { 0.10000000000000001, 0, 3u },
  { 0.25, 0, 7u },
  { 0.33329999999999999, 0, 10u },
  { 0.5, 0, 15u },
  { 0.61803398870000004, 0, 19u },
  { 0.75, 0, 23u },
  { 0.90000000000000002, 0, 27u },
  { 0.999, 0, 30u },
  { 1, 0, 30u },
  { 0, 0, 0u },
  { 0.5, 0, 15u },
  { 0, 0, 0u }
};
#define GP_B0_INIT { "empty-spine.epub", 0, 1, GP_B0_NAMES, GP_B0_LEN, GP_B0_IDS, 2, GP_B0_FR, 6, GP_B0_LOC, 12, 0, 15u, 0.4838709677419355 }
static const char* const GP_B1_NAMES[] = { "OEBPS/text/one.xhtml", "OEBPS/text/two.xhtml", "" };
static const uint32_t GP_B1_LEN[] = { 28u, 28u, 0u };
static const char* const GP_B1_IDS[] = { "id:isbn-9780000000001", "ta:deep-in-a-folder-grace-hopper", "fp:f43fde116510c81e", "" };
static const GpFrac GP_B1_FR[] = {
  { 0, 0u, 0 },
  { 0, 9u, 0.16071428571428573 },
  { 0, 14u, 0.25 },
  { 0, 27u, 0.48214285714285715 },
  { 0, 28u, 0.5 },
  { 0, 35u, 0.5 },
  { 1, 0u, 0.5 },
  { 1, 9u, 0.6607142857142857 },
  { 1, 14u, 0.75 },
  { 1, 27u, 0.98214285714285721 },
  { 1, 28u, 1 },
  { 1, 35u, 1 },
  { 0, 0u, 0 }
};
static const GpLoc GP_B1_LOC[] = {
  { 0, 0, 0u },
  { 0.10000000000000001, 0, 5u },
  { 0.25, 0, 14u },
  { 0.33329999999999999, 0, 18u },
  { 0.5, 1, 0u },
  { 0.61803398870000004, 1, 6u },
  { 0.75, 1, 14u },
  { 0.90000000000000002, 1, 22u },
  { 0.999, 1, 27u },
  { 1, 1, 27u },
  { 0, 0, 0u },
  { 0.25, 0, 14u },
  { 0.5, 1, 0u },
  { 0.75, 1, 14u },
  { 0, 0, 0u }
};
#define GP_B1_INIT { "epub2-subdir.epub", 0, 2, GP_B1_NAMES, GP_B1_LEN, GP_B1_IDS, 3, GP_B1_FR, 12, GP_B1_LOC, 14, 1, 14u, 0.75 }
static const char* const GP_B2_NAMES[] = { "ch1.xhtml", "ch2.xhtml", "" };
static const uint32_t GP_B2_LEN[] = { 72u, 61u, 0u };
static const char* const GP_B2_IDS[] = { "id:urn-uuid-12345678-abcd", "ta:the-long-winding-road-ada-lovelace", "fp:df7a0e252d029c6c", "" };
static const GpFrac GP_B2_FR[] = {
  { 0, 0u, 0 },
  { 0, 24u, 0.16666666666666666 },
  { 0, 36u, 0.25 },
  { 0, 71u, 0.49305555555555558 },
  { 0, 72u, 0.5 },
  { 0, 79u, 0.5 },
  { 1, 0u, 0.5 },
  { 1, 20u, 0.66393442622950816 },
  { 1, 30u, 0.74590163934426235 },
  { 1, 60u, 0.99180327868852458 },
  { 1, 61u, 1 },
  { 1, 68u, 1 },
  { 0, 0u, 0 }
};
static const GpLoc GP_B2_LOC[] = {
  { 0, 0, 0u },
  { 0.10000000000000001, 0, 14u },
  { 0.25, 0, 36u },
  { 0.33329999999999999, 0, 47u },
  { 0.5, 1, 0u },
  { 0.61803398870000004, 1, 14u },
  { 0.75, 1, 30u },
  { 0.90000000000000002, 1, 48u },
  { 0.999, 1, 60u },
  { 1, 1, 60u },
  { 0, 0, 0u },
  { 0.25, 0, 36u },
  { 0.5, 1, 0u },
  { 0.75, 1, 30u },
  { 0, 0, 0u }
};
#define GP_B2_INIT { "epub3-nav.epub", 0, 2, GP_B2_NAMES, GP_B2_LEN, GP_B2_IDS, 3, GP_B2_FR, 12, GP_B2_LOC, 14, 1, 30u, 0.74590163934426235 }
static const char* const GP_B3_NAMES[] = { "OEBPS/Text/front.xhtml", "OEBPS/Text/one.xhtml", "OEBPS/Text/three.xhtml", "OEBPS/Text/four.xhtml", "" };
static const uint32_t GP_B3_LEN[] = { 7u, 2334u, 77703u, 627u, 0u };
static const char* const GP_B3_IDS[] = { "id:urn-uuid-kosync-mixed-0001", "ta:timber-line-a-hunter", "fp:60f2b4ac76dac1e4", "" };
static const GpFrac GP_B3_FR[] = {
  { 0, 0u, 0 },
  { 0, 2u, 0.071428571428571425 },
  { 0, 3u, 0.10714285714285714 },
  { 0, 6u, 0.21428571428571427 },
  { 0, 7u, 0.25 },
  { 0, 14u, 0.25 },
  { 1, 0u, 0.25 },
  { 1, 778u, 0.33333333333333331 },
  { 1, 1167u, 0.375 },
  { 1, 2333u, 0.49989288774635821 },
  { 1, 2334u, 0.5 },
  { 1, 2341u, 0.5 },
  { 2, 0u, 0.5 },
  { 2, 25901u, 0.58333333333333337 },
  { 2, 38851u, 0.62499839131050283 },
  { 2, 77702u, 0.74999678262100566 },
  { 2, 77703u, 0.75 },
  { 2, 77710u, 0.75 },
  { 3, 0u, 0.75 },
  { 3, 209u, 0.83333333333333337 },
  { 3, 313u, 0.87480063795853269 },
  { 3, 626u, 0.99960127591706538 },
  { 3, 627u, 1 },
  { 3, 634u, 1 },
  { 0, 0u, 0 }
};
static const GpLoc GP_B3_LOC[] = {
  { 0, 0, 0u },
  { 0.10000000000000001, 0, 2u },
  { 0.25, 1, 0u },
  { 0.33329999999999999, 1, 777u },
  { 0.5, 2, 0u },
  { 0.61803398870000004, 2, 36686u },
  { 0.75, 3, 0u },
  { 0.90000000000000002, 3, 376u },
  { 0.999, 3, 624u },
  { 1, 3, 626u },
  { 0, 0, 0u },
  { 0.125, 0, 3u },
  { 0.25, 1, 0u },
  { 0.375, 1, 1167u },
  { 0.5, 2, 0u },
  { 0.625, 2, 38851u },
  { 0.75, 3, 0u },
  { 0.875, 3, 313u },
  { 0, 0, 0u }
};
#define GP_B3_INIT { "kosync-mixed.epub", 0, 4, GP_B3_NAMES, GP_B3_LEN, GP_B3_IDS, 3, GP_B3_FR, 24, GP_B3_LOC, 18, 2, 38851u, 0.62499839131050283 }
static const char* const GP_B4_NAMES[] = { "a.xhtml", "b.xhtml", "c.xhtml", "" };
static const uint32_t GP_B4_LEN[] = { 315u, 3124u, 1024u, 0u };
static const char* const GP_B4_IDS[] = { "ta:plain", "fp:109f21b18c7a0d44", "" };
static const GpFrac GP_B4_FR[] = {
  { 0, 0u, 0 },
  { 0, 105u, 0.1111111111111111 },
  { 0, 157u, 0.16613756613756614 },
  { 0, 314u, 0.33227513227513228 },
  { 0, 315u, 0.33333333333333331 },
  { 0, 322u, 0.33333333333333331 },
  { 1, 0u, 0.33333333333333331 },
  { 1, 1041u, 0.44440887750746905 },
  { 1, 1562u, 0.5 },
  { 1, 3123u, 0.66655996585574051 },
  { 1, 3124u, 0.66666666666666663 },
  { 1, 3131u, 0.66666666666666663 },
  { 2, 0u, 0.66666666666666663 },
  { 2, 341u, 0.77766927083333337 },
  { 2, 512u, 0.83333333333333337 },
  { 2, 1023u, 0.99967447916666663 },
  { 2, 1024u, 1 },
  { 2, 1031u, 1 },
  { 0, 0u, 0 }
};
static const GpLoc GP_B4_LOC[] = {
  { 0, 0, 0u },
  { 0.10000000000000001, 0, 94u },
  { 0.25, 0, 236u },
  { 0.33329999999999999, 0, 314u },
  { 0.5, 1, 1562u },
  { 0.61803398870000004, 1, 2668u },
  { 0.75, 2, 256u },
  { 0.90000000000000002, 2, 716u },
  { 0.999, 2, 1020u },
  { 1, 2, 1023u },
  { 0, 0, 0u },
  { 0.16666666666666666, 0, 157u },
  { 0.33333333333333331, 1, 0u },
  { 0.5, 1, 1562u },
  { 0.66666666666666663, 2, 0u },
  { 0.83333333333333337, 2, 512u },
  { 0, 0, 0u }
};
#define GP_B4_INIT { "kosync-plain.epub", 0, 3, GP_B4_NAMES, GP_B4_LEN, GP_B4_IDS, 2, GP_B4_FR, 18, GP_B4_LOC, 16, 1, 1562u, 0.5 }
static const char* const GP_B5_NAMES[] = { "my-notes.txt", "" };
static const uint32_t GP_B5_LEN[] = { 53u, 0u };
static const char* const GP_B5_IDS[] = { "ta:my-notes", "fp:6c5ac77702fc000e", "" };
static const GpFrac GP_B5_FR[] = {
  { 0, 0u, 0 },
  { 0, 17u, 0.32075471698113206 },
  { 0, 26u, 0.49056603773584906 },
  { 0, 52u, 0.98113207547169812 },
  { 0, 53u, 1 },
  { 0, 60u, 1 },
  { 0, 0u, 0 }
};
static const GpLoc GP_B5_LOC[] = {
  { 0, 0, 0u },
  { 0.10000000000000001, 0, 5u },
  { 0.25, 0, 13u },
  { 0.33329999999999999, 0, 17u },
  { 0.5, 0, 26u },
  { 0.61803398870000004, 0, 32u },
  { 0.75, 0, 39u },
  { 0.90000000000000002, 0, 47u },
  { 0.999, 0, 52u },
  { 1, 0, 52u },
  { 0, 0, 0u },
  { 0.5, 0, 26u },
  { 0, 0, 0u }
};
#define GP_B5_INIT { "my-notes.txt", 0, 1, GP_B5_NAMES, GP_B5_LEN, GP_B5_IDS, 2, GP_B5_FR, 6, GP_B5_LOC, 12, 0, 26u, 0.49056603773584906 }
static const char* const GP_B6_NAMES[] = { "c.xhtml", "" };
static const uint32_t GP_B6_LEN[] = { 18u, 0u };
static const char* const GP_B6_IDS[] = { "ta:no-metadata", "fp:65c3e84f189eeead", "" };
static const GpFrac GP_B6_FR[] = {
  { 0, 0u, 0 },
  { 0, 6u, 0.33333333333333331 },
  { 0, 9u, 0.5 },
  { 0, 17u, 0.94444444444444442 },
  { 0, 18u, 1 },
  { 0, 25u, 1 },
  { 0, 0u, 0 }
};
static const GpLoc GP_B6_LOC[] = {
  { 0, 0, 0u },
  { 0.10000000000000001, 0, 1u },
  { 0.25, 0, 4u },
  { 0.33329999999999999, 0, 5u },
  { 0.5, 0, 9u },
  { 0.61803398870000004, 0, 11u },
  { 0.75, 0, 13u },
  { 0.90000000000000002, 0, 16u },
  { 0.999, 0, 17u },
  { 1, 0, 17u },
  { 0, 0, 0u },
  { 0.5, 0, 9u },
  { 0, 0, 0u }
};
#define GP_B6_INIT { "no-metadata.epub", 0, 1, GP_B6_NAMES, GP_B6_LEN, GP_B6_IDS, 2, GP_B6_FR, 6, GP_B6_LOC, 12, 0, 9u, 0.5 }
static const GpBook GP_BOOKS[] = {
  GP_B0_INIT,
  GP_B1_INIT,
  GP_B2_INIT,
  GP_B3_INIT,
  GP_B4_INIT,
  GP_B5_INIT,
  GP_B6_INIT,
};
// /books/positions.cbs as the 0.9.78 store wrote it, one saved place per book above.
static const char GP_STORE_BLOB[] =
  "CBSTORE1\n"
  "0\x09""15\x09""0.483871\x09""1790000000\x09""ta:no-spine-at-all,fp:8ba8cf95cb4411da\n"
  "1\x09""14\x09""0.750000\x09""1790000001\x09""id:isbn-9780000000001,ta:deep-in-a-folder-grace-hopper,fp:f43fde116510c81e\n"
  "1\x09""30\x09""0.745902\x09""1790000002\x09""id:urn-uuid-12345678-abcd,ta:the-long-winding-road-ada-lovelace,fp:df7a0e252d029c6c\n"
  "2\x09""38851\x09""0.624998\x09""1790000003\x09""id:urn-uuid-kosync-mixed-0001,ta:timber-line-a-hunter,fp:60f2b4ac76dac1e4\n"
  "1\x09""1562\x09""0.500000\x09""1790000004\x09""ta:plain,fp:109f21b18c7a0d44\n"
  "0\x09""26\x09""0.490566\x09""1790000005\x09""ta:my-notes,fp:6c5ac77702fc000e\n"
  "0\x09""9\x09""0.500000\x09""1790000006\x09""ta:no-metadata,fp:65c3e84f189eeead\n"
  ;
