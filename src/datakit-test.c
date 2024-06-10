/* Allow test prototypes to exist from headers at edit time
 * in addition to at compile time. */
#ifndef DATAKIT_TEST
#define DATAKIT_TEST
#endif

/* Begin */
#include "datakit.h"

/* Add all includes here so we get their test main functions defined. */
#include "flex.h"
#include "mds.h"
#include "mdsc.h"
#include "mflex.h"
#include "multilist.h"
#include "multilistFull.h"
#include "multilistMedium.h"
#include "multilistSmall.h"
// #include "quickmap.h"
// #include "quickcount.h"
// #include "intset.h"
#include "hyperloglog.h"
// #include "multidictClassMap.h"
// #include "multidictClassAtom.h"
// #include "patternTrie.h"
#include "../deps/sha1/sha1.h"
#include "databox.h"
#include "databoxLinear.h"
#include "dataspeed.h"
#include "dj.h"
#include "dod.h"
#include "fibbuf.h"
#include "float16.h"
#include "intset.h"
#include "intsetU32.h"
#include "jebuf.h"
#include "membound.h"
#include "multiarray.h"
#include "multiarrayLarge.h"
#include "multiarrayMedium.h"
#include "multiarraySmall.h"
#include "multilru.h"
#include "multimap.h"
#include "multimapAtom.h"
#include "multimapFull.h"
#include "multimapMedium.h"
#include "multimapSmall.h"
#include "multiroar.h"
#include "offsetArray.h"
#include "ptrPrevNext.h"
#include "str.h"
#include "strDoubleFormat.h"
#include "util.h"
#include "xof.h"

#if __x86_64__
#define USE_INTSET_BIG 1
#include "intsetBig.h"
#endif

#include <strings.h>
int main(int argc, char *argv[]) {
    /* Don't break the databox size contract! */
    assert(sizeof(databox) == 16);

    if (argc >= 3 && !strcasecmp(argv[1], "test")) {
        if (!strcasecmp(argv[2], "flex")) {
            return flexTest(argc, argv);
        } else if (!strcasecmp(argv[2], "f16") ||
                   !strcasecmp(argv[2], "float16")) {
            return float16Test(argc, argv);
        } else if (!strcasecmp(argv[2], "intset")) {
            return intsetTest(argc, argv);
        } else if (!strcasecmp(argv[2], "intsetU32")) {
            return intsetU32Test(argc, argv);
#if USE_INTSET_BIG
        } else if (!strcasecmp(argv[2], "intsetBig")) {
            return intsetBigTest(argc, argv);
#endif
        } else if (!strcasecmp(argv[2], "dj")) {
            return djTest(argc, argv);
        } else if (!strcasecmp(argv[2], "multilistFull")) {
            return multilistFullTest(argc, argv);
        } else if (!strcasecmp(argv[2], "multilist") ||
                   !strcasecmp(argv[2], "ml")) {
            return multilistTest(argc, argv);
        } else if (!strcasecmp(argv[2], "util")) {
            return utilTest(argc, argv);
        } else if (!strcasecmp(argv[2], "allstr")) {
            return mdsTest(argc, argv) + mdsBenchMain() + mdscTest(argc, argv) +
                   mdscBenchMain();
        } else if (!strcasecmp(argv[2], "mds")) {
            return mdsTest(argc, argv);
        } else if (!strcasecmp(argv[2], "mdsbench")) {
            return mdsBenchMain();
        } else if (!strcasecmp(argv[2], "mdsc")) {
            return mdscTest(argc, argv);
        } else if (!strcasecmp(argv[2], "mdscbench")) {
            return mdscBenchMain();
        } else if (!strcasecmp(argv[2], "offsetArray")) {
            return offsetArrayTest(argc, argv);
        } else if (!strcasecmp(argv[2], "hll") ||
                   !strcasecmp(argv[2], "hyperloglog")) {
            return hyperloglogTest(argc, argv);
#if 0
        } else if (!strcasecmp(argv[2], "endianconv")) {
            return endianconvTest(argc, argv);
        } else if (!strcasecmp(argv[2], "multidictClassMap")) {
            return multidictClassMapTest(argc, argv);
        } else if (!strcasecmp(argv[2], "patternTrie")) {
            return patternTrieTest(argc, argv);
        } else if (!strcasecmp(argv[2], "multidictClassAtom")) {
            return multidictClassAtomTest(argc, argv);
        } else if (!strcasecmp(argv[2], "quickcount")) {
            return quickcountTest(argc, argv);
        } else if (!strcasecmp(argv[2], "quickmap")) {
            return quickmapTest(argc, argv);
#endif
        } else if (!strcasecmp(argv[2], "sha1")) {
            return sha1Test(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "multimapFull")) {
            return multimapFullTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "multimap")) {
            return multimapTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "multiarrayLarge")) {
            return multiarrayLargeTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "multiarraySmall")) {
            return multiarraySmallTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "multiarrayMedium")) {
            return multiarrayMediumTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "multiarray")) {
            return multiarrayTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "databox")) {
            return databoxTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "str")) {
            return strTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "strDoubleFormat")) {
            return strDoubleFormatTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "multiroar")) {
            return multiroarTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "dod")) {
            return dodTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "xof")) {
            return xofTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "fibbuf")) {
            return fibbufTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "jebuf")) {
            return jebufTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "mflex")) {
            return mflexTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "membound")) {
            return memboundTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "speed")) {
            return dataspeed(atoi(argv[3]), atoi(argv[4]));
        } else if (!strcasecmp(argv[2], "databoxLinear")) {
            return databoxLinearTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "multimapatom") ||
                   !strcasecmp(argv[2], "atom")) {
            return multimapAtomTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "lru") ||
                   !strcasecmp(argv[2], "mlru") ||
                   !strcasecmp(argv[2], "multilru")) {
            return multilruTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "ppn") ||
                   !strcasecmp(argv[2], "ptrprevnext")) {
            return ptrPrevNextTest(argc - 2, argv + 2);
        } else if (!strcasecmp(argv[2], "ALL")) {
            uint32_t result = 0;
            result += flexTest(argc, argv);
            result += multilistFullTest(argc, argv);
            result += utilTest(argc, argv);
            result += mdsTest(argc, argv);
            result += mdscTest(argc, argv);
            result += hyperloglogTest(argc, argv);
            result += offsetArrayTest(argc, argv);
#if 0
            result += intsetTest(argc, argv);
            result += endianconvTest(argc, argv);
            result += patternTrieTest(argc, argv);
            result += multidictClassAtomTest(argc, argv);
            result += quickcountTest(argc, argv);
            result += quickmapTest(argc, argv);
#endif
            result += multimapFullTest(argc, argv);
            result += multimapTest(argc, argv);
            result += multiarrayLargeTest(argc, argv);
            result += multiarraySmallTest(argc, argv);
            result += multiarrayMediumTest(argc, argv);
            result += multiarrayTest(argc, argv);
            result += databoxTest(argc, argv);
            result += strTest(argc, argv);
            result += strDoubleFormatTest(argc, argv);
            result += multiroarTest(argc, argv);
            result += dodTest(argc, argv);
            result += xofTest(argc, argv);
            result += fibbufTest(argc, argv);
            result += jebufTest(argc, argv);
            result += mflexTest(argc, argv);
            result += memboundTest(argc, argv);
            result += multilruTest(argc, argv);
            result += ptrPrevNextTest(argc, argv);
            result += multimapAtomTest(argc, argv);
            result += databoxLinearTest(argc, argv);
            return result;
        }
    }

    printf("Test not found!\n");
    return -3;
}
