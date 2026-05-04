#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <time.h>

#define MAX_PROCESSES  3
#define MAX_PTE        524288   /* 512K virtual pages (31-bit VA) */
#define PAGE_SIZE      4096U
#define LINE_BUFFER    512
#define DATA_BUS_BYTES 4
#define HIT_CYCLES     1
#define MISS_BASE      4        /* cycles per memory-bus read */
#define EXEC_CYCLES    2        /* instruction execution overhead */
#define EA_CYCLES      1        /* effective-address overhead */
#define PF_CYCLES      100

/* ---- structures ---- */

typedef struct {
    int          valid;
    unsigned int tag;
} CacheBlock;

typedef struct {
    CacheBlock *ways;
    int         rr_next;
} CacheRow;

typedef struct {
    long long totalCacheAccesses;
    long long instrBytes;
    long long srcDstBytes;
    long long cacheHits;
    long long cacheMisses;
    long long compulsoryMisses;
    long long conflictMisses;
    long long instrCount;
    double    totalCycles;   /* FIX 5 */
} CacheStats;

typedef struct {
    /* inputs */
    int   cacheSizeKB;
    int   blockSize;
    int   associativity;
    char  replacement[5];
    int   physicalMemMB;
    float percentOS;
    int   instructions;
    int   numFiles;
    char  traceFiles[3][256];
    /* cache calculated */
    int       totalBlocks;
    int       totalRows;
    int       indexBits;
    int       offsetBits;
    int       tagBits;
    long long overheadBytes;           /* FIX 9 */
    long long implementationSizeBytes; /* FIX 9 */
    double    cost;
    /* physical memory calculated */
    long long physicalPages;
    long long systemPages;
    int       pageTableEntryBits;
    long long totalPageTableBytes;
    /* VM simulation results */
    long long virtualPagesMapped;  /* FIX 1: unique new pages only */
    long long pageTableHits;
    long long pagesFromFree;
    long long pageFaults;
    int       usedEntries[MAX_PROCESSES];
    long long pageTableWasted[MAX_PROCESSES];
    /* cache simulation results */
    CacheStats cacheStats;
} Config;

typedef struct {
    int valid;
    int physical_frame;
} PTE;

typedef struct {
    char *filename;
    PTE  *page_table;
    int   used_entries;
    int   finished;
} Process;

typedef struct {
    int          in_use;
    int          owner_pid;
    unsigned int owner_vpn;
} PhysicalPage;

/* ---- cache helpers ---- */

static CacheRow *createCache(int totalRows, int associativity)
{
    int i, j;
    CacheRow *cache = calloc(totalRows, sizeof(CacheRow));
    if (!cache) { fprintf(stderr,"OOM cache rows\n"); exit(1); }
    for (i = 0; i < totalRows; i++) {
        cache[i].ways = calloc(associativity, sizeof(CacheBlock));
        if (!cache[i].ways) { fprintf(stderr,"OOM cache ways\n"); exit(1); }
        cache[i].rr_next = 0;
        for (j = 0; j < associativity; j++) {
            cache[i].ways[j].valid = 0;
            cache[i].ways[j].tag   = 0;
        }
    }
    return cache;
}

static void destroyCache(CacheRow *cache, int totalRows)
{
    int i;
    for (i = 0; i < totalRows; i++) free(cache[i].ways);
    free(cache);
}

/* FIX 12: returns cycles; FIX 8: RND implemented */
static double accessCacheRow(unsigned int rowIndex, unsigned int tag,
                              int associativity, CacheRow *cache,
                              CacheStats *stats, const char *replacement,
                              int readsToFill)
{
    int w;
    CacheRow *row = &cache[rowIndex];
    stats->totalCacheAccesses++;

    for (w = 0; w < associativity; w++) {
        if (row->ways[w].valid && row->ways[w].tag == tag) {
            stats->cacheHits++;
            return HIT_CYCLES;
        }
    }

    stats->cacheMisses++;
    double cycles = (double)(readsToFill * MISS_BASE);

    for (w = 0; w < associativity; w++) {
        if (!row->ways[w].valid) {
            stats->compulsoryMisses++;
            row->ways[w].valid = 1;
            row->ways[w].tag   = tag;
            return cycles;
        }
    }

    stats->conflictMisses++;
    int victim;
    if (strcmp(replacement,"RND")==0 || strcmp(replacement,"rnd")==0) {
        victim = rand() % associativity;   /* FIX 8 */
    } else {
        victim = row->rr_next;
        row->rr_next = (row->rr_next + 1) % associativity;
    }
    row->ways[victim].tag   = tag;
    row->ways[victim].valid = 1;
    return cycles;
}

static double accessCache(unsigned int addr, int numBytes,
                           int indexBits, int offsetBits,
                           int totalRows, int associativity,
                           CacheRow *cache, CacheStats *stats,
                           const char *replacement, int readsToFill)
{
    if (addr == 0) return 0.0;
    double cycles = 0.0;
    unsigned int startBlock = addr >> offsetBits;
    unsigned int endBlock   = (addr + numBytes - 1) >> offsetBits;
    unsigned int block;
    for (block = startBlock; block <= endBlock; block++) {
        unsigned int rowIndex = block & ((1u << indexBits) - 1);
        unsigned int tag      = block >> indexBits;
        if ((int)rowIndex >= totalRows)
            rowIndex = rowIndex % (unsigned)totalRows;
        cycles += accessCacheRow(rowIndex, tag, associativity,
                                 cache, stats, replacement, readsToFill);
    }
    return cycles;
}

/* FIX 13: invalidate cache lines belonging to a physical page */
static void cacheEvictPage(CacheRow *cache, unsigned int physPage,
                            int offsetBits, int indexBits,
                            int totalRows, int associativity)
{
    unsigned int base = physPage << 12;  /* page base physical address */
    unsigned int bs   = 1u << offsetBits;
    unsigned int a;
    for (a = 0; a < PAGE_SIZE; a += bs) {
        unsigned int addr     = base + a;
        unsigned int block    = addr >> offsetBits;
        unsigned int rowIndex = block & ((1u << indexBits) - 1);
        unsigned int tag      = block >> indexBits;
        int w;
        if ((int)rowIndex >= totalRows)
            rowIndex = rowIndex % (unsigned)totalRows;
        CacheRow *row = &cache[rowIndex];
        for (w = 0; w < associativity; w++) {
            if (row->ways[w].valid && row->ways[w].tag == tag) {
                row->ways[w].valid = 0;
                break;
            }
        }
    }
}

/* ---- VM translation ---- */

/* FIX 1+13: virtualPagesMapped = unique new pages only; cache evicted on fault */
static unsigned int translateAddress(unsigned int addr, int pid,
                                      Process *processes,
                                      PhysicalPage *phys_pages,
                                      long long user_pages,
                                      Config *config,
                                      CacheRow *cache,
                                      int *out_pf)
{
    unsigned int vpn    = addr >> 12;
    unsigned int offset = addr & 0xFFF;
    *out_pf = 0;
    if (vpn >= MAX_PTE) return 0;
    
     config->virtualPagesMapped++; 

    if (processes[pid].page_table[vpn].valid) {
        config->pageTableHits++;
        return ((unsigned int)processes[pid].page_table[vpn].physical_frame
                << 12) | offset;
    }

    /* New page: find a frame */
    //config->virtualPagesMapped++;   /* FIX 1 */
    processes[pid].used_entries++;

    long long j;
    int found = -1;
    for (j = 0; j < user_pages; j++) {
        if (!phys_pages[j].in_use) { found = (int)j; break; }
    }

    if (found != -1) {
        processes[pid].page_table[vpn].valid          = 1;
        processes[pid].page_table[vpn].physical_frame = found;
        phys_pages[found].in_use    = 1;
        phys_pages[found].owner_pid = pid;
        phys_pages[found].owner_vpn = vpn;
        config->pagesFromFree++;
        return ((unsigned int)found << 12) | offset;
    }

    /* Page fault: round-robin eviction */
    *out_pf = 1;
    config->pageFaults++;
    static long long rr_ptr = 0;
    int victim = (int)(rr_ptr % user_pages);
    rr_ptr++;

    /* FIX 13: invalidate cache lines of victim frame */
    cacheEvictPage(cache, (unsigned int)victim,
                   config->offsetBits, config->indexBits,
                   config->totalRows,  config->associativity);

    int old_pid = phys_pages[victim].owner_pid;
    int old_vpn = (int)phys_pages[victim].owner_vpn;
    processes[old_pid].page_table[old_vpn].valid = 0;
    processes[old_pid].used_entries--;

    processes[pid].page_table[vpn].valid          = 1;
    processes[pid].page_table[vpn].physical_frame = victim;
    phys_pages[victim].owner_pid = pid;
    phys_pages[victim].owner_vpn = vpn;

    return ((unsigned int)victim << 12) | offset;
}

/* ---- main simulation ---- */

void simulateVirtualMemory(Config *config)
{
    int i;
    srand((unsigned)time(NULL));

    Process processes[MAX_PROCESSES];
    memset(processes, 0, sizeof(processes));

    long long user_pages = config->physicalPages - config->systemPages;
    if (user_pages <= 0) user_pages = 1;

    PhysicalPage *phys_pages = calloc(user_pages, sizeof(PhysicalPage));
    if (!phys_pages) { fprintf(stderr,"OOM phys_pages\n"); exit(1); }

    for (i = 0; i < config->numFiles; i++) {
        processes[i].filename   = config->traceFiles[i];
        processes[i].page_table = calloc(MAX_PTE, sizeof(PTE));
        if (!processes[i].page_table) { fprintf(stderr,"OOM PTE\n"); exit(1); }
    }
    for (i = 0; i < user_pages; i++) {
        phys_pages[i].in_use    = 0;
        phys_pages[i].owner_pid = -1;
    }

    /* FIX 3: ONE shared cache */
    CacheRow  *cache = createCache(config->totalRows, config->associativity);
    CacheStats *cs   = &config->cacheStats;
    memset(cs, 0, sizeof(CacheStats));

    int readsToFill = (config->blockSize + DATA_BUS_BYTES - 1) / DATA_BUS_BYTES;

    FILE *fps[MAX_PROCESSES] = {NULL};
    for (i = 0; i < config->numFiles; i++) {
        fps[i] = fopen(config->traceFiles[i], "r");
        if (!fps[i]) processes[i].finished = 1;
    }

    char line[LINE_BUFFER];
    int  allDone = 0;

    while (!allDone) {
        allDone = 1;
        for (i = 0; i < config->numFiles; i++) {
            if (processes[i].finished) continue;
            allDone = 0;

            int instrDone = 0;
            int limit = (config->instructions == -1) ? INT_MAX : config->instructions;

            while (instrDone < limit) {
                /* Find next EIP line */
                int gotEIP = 0;
                while (fgets(line, sizeof(line), fps[i])) {
                    if (strncmp(line, "EIP", 3) == 0) { gotEIP = 1; break; }
                }
                if (!gotEIP) {
                    /* Process done: FIX 13 free pages and invalidate cache */
                    int v;
                    for (v = 0; v < MAX_PTE; v++) {
                        if (processes[i].page_table[v].valid) {
                            int pp = processes[i].page_table[v].physical_frame;
                            cacheEvictPage(cache, (unsigned int)pp,
                                           config->offsetBits, config->indexBits,
                                           config->totalRows, config->associativity);
                            phys_pages[pp].in_use    = 0;
                            phys_pages[pp].owner_pid = -1;
                            processes[i].page_table[v].valid = 0;
                        }
                    }
                    fclose(fps[i]); fps[i] = NULL;
                    processes[i].finished = 1;
                    break;
                }

                /* Parse EIP */
                unsigned int eipAddr = 0;
                int          eipLen  = 0;
                if (sscanf(line, "EIP (%d): %x", &eipLen, &eipAddr) != 2) {
                    instrDone++; continue;
                }

                int pf = 0;
                unsigned int physEIP = translateAddress(eipAddr, i, processes,
                                                        phys_pages, user_pages,
                                                        config, cache, &pf);
                double icycles = 0.0;
                if (physEIP) {
                    icycles = accessCache(physEIP, eipLen,
                                          config->indexBits, config->offsetBits,
                                          config->totalRows, config->associativity,
                                          cache, cs, config->replacement, readsToFill);
                }
                icycles += EXEC_CYCLES;
                if (pf) icycles += PF_CYCLES;
                cs->totalCycles += icycles;
                cs->instrBytes  += eipLen;
                cs->instrCount++;

                /* Parse dstM / srcM line */
                if (fgets(line, sizeof(line), fps[i])) {

                    /* FIX 2+4: dst cache access */
                    char *dstPtr = strstr(line, "dstM:");
                    if (dstPtr) {
                        unsigned int dstAddr = 0; char dstData[16]={0};
                        sscanf(dstPtr + 6, "%x %15s", &dstAddr, dstData);
                        if (dstAddr != 0 && dstData[0] != '-') {
                            pf = 0;
                            unsigned int phD = translateAddress(dstAddr, i,
                                processes, phys_pages, user_pages,
                                config, cache, &pf);
                            double dc = 0.0;
                            if (phD) dc = accessCache(phD, 4,
                                config->indexBits, config->offsetBits,
                                config->totalRows, config->associativity,
                                cache, cs, config->replacement, readsToFill);
                            dc += EA_CYCLES;
                            if (pf) dc += PF_CYCLES;
                            cs->totalCycles += dc;
                            cs->srcDstBytes += 4;  /* FIX 4 */
                        }
                    }

                    /* FIX 2+4: src cache access */
                    char *srcPtr = strstr(line, "srcM:");
                    if (srcPtr) {
                        unsigned int srcAddr = 0; char srcData[16]={0};
                        sscanf(srcPtr + 6, "%x %15s", &srcAddr, srcData);
                        if (srcAddr != 0 && srcData[0] != '-') {
                            pf = 0;
                            unsigned int phS = translateAddress(srcAddr, i,
                                processes, phys_pages, user_pages,
                                config, cache, &pf);
                            double sc = 0.0;
                            if (phS) sc = accessCache(phS, 4,
                                config->indexBits, config->offsetBits,
                                config->totalRows, config->associativity,
                                cache, cs, config->replacement, readsToFill);
                            sc += EA_CYCLES;
                            if (pf) sc += PF_CYCLES;
                            cs->totalCycles += sc;
                            cs->srcDstBytes += 4;  /* FIX 4 */
                        }
                    }
                }

                instrDone++;
            }
        }
    }

    destroyCache(cache, config->totalRows);

    /* FIX 6: wasted = unused entries * ceiling bytes per PTE */
    int pte_bytes = (config->pageTableEntryBits + 7) / 8;
    for (i = 0; i < config->numFiles; i++) {
        config->usedEntries[i]     = processes[i].used_entries;
        config->pageTableWasted[i] =
            (long long)(MAX_PTE - processes[i].used_entries) * config->pageTableEntryBits / 8;
        if (processes[i].page_table) free(processes[i].page_table);
    }
    free(phys_pages);
}

/* ---- argument parsing ---- */

void parseArguments(int argc, char *argv[], Config *config)
{
    int i;
    for (i = 1; i < argc; i++) {
        if      (strcmp(argv[i],"-s")==0 && i+1<argc) config->cacheSizeKB    = atoi(argv[++i]);
        else if (strcmp(argv[i],"-b")==0 && i+1<argc) config->blockSize       = atoi(argv[++i]);
        else if (strcmp(argv[i],"-a")==0 && i+1<argc) config->associativity   = atoi(argv[++i]);
        else if (strcmp(argv[i],"-r")==0 && i+1<argc) { strncpy(config->replacement,argv[++i],4); config->replacement[4]='\0'; }
        else if (strcmp(argv[i],"-p")==0 && i+1<argc) config->physicalMemMB   = atoi(argv[++i]);
        else if (strcmp(argv[i],"-u")==0 && i+1<argc) config->percentOS       = (float)atof(argv[++i]);
        else if (strcmp(argv[i],"-n")==0 && i+1<argc) config->instructions    = atoi(argv[++i]);
        else if (strcmp(argv[i],"-f")==0 && i+1<argc && config->numFiles<3) {
            strncpy(config->traceFiles[config->numFiles++], argv[++i], 255);
        }
    }
}

/* ---- calculated values ---- */

void computeCacheValues(Config *config)
{
    long long cacheBytes = (long long)config->cacheSizeKB * 1024;
    config->totalBlocks  = (int)(cacheBytes / config->blockSize);
    config->totalRows    = (int)(cacheBytes / ((long long)config->associativity * config->blockSize));
    config->offsetBits   = (int)log2(config->blockSize);
    config->indexBits    = (int)log2(config->totalRows);

    long long physBytes  = (long long)config->physicalMemMB * 1024 * 1024;
    int physBits         = (int)log2(physBytes);
    config->tagBits      = physBits - config->indexBits - config->offsetBits;
    if (config->tagBits < 0) config->tagBits = 0;

    /* spec overhead formula: (1+tagBits)*totalBlocks / 8 */
    config->overheadBytes = (long long)(1 + config->tagBits) * config->totalBlocks / 8;
    config->implementationSizeBytes = cacheBytes + config->overheadBytes;
    config->cost = (config->implementationSizeBytes / 1024.0) * 0.07;
}

void computeMemoryValues(Config *config)
{
    long long physBytes      = (long long)config->physicalMemMB * 1024 * 1024;
    config->physicalPages    = physBytes / PAGE_SIZE;
    config->systemPages      = (long long)(config->percentOS / 100.0 * config->physicalPages);
    int physPageBits         = (int)log2((double)config->physicalPages);
    config->pageTableEntryBits = 1 + physPageBits;
    config->totalPageTableBytes = (long long)MAX_PTE * config->numFiles * config->pageTableEntryBits / 8;
}

/* ---- output ---- */

void printResults(Config *config)
{
    int i;
    CacheStats *cs = &config->cacheStats;

    printf("Cache Simulator - CS 3853 - Team #2\n\n");
    printf("Trace File(s):\n");
    for (i = 0; i < config->numFiles; i++)
        printf("   %s\n", config->traceFiles[i]);

    /* Milestone 1: Cache Input Parameters */
    printf("\n***** Cache Input Parameters *****\n\n");
    printf("Cache Size:                    %d KB\n",    config->cacheSizeKB);
    printf("Block Size:                    %d bytes\n", config->blockSize);
    printf("Associativity:                 %d\n",       config->associativity);
    if (strcmp(config->replacement,"RR")==0||strcmp(config->replacement,"rr")==0)
        printf("Replacement Policy:            Round Robin\n");
    else
        printf("Replacement Policy:            Random\n");
    printf("Physical Memory:               %d MB\n",   config->physicalMemMB);
    printf("Percent Memory Used by System: %.1f%%\n",  config->percentOS);
    if (config->instructions == -1)
        printf("Instructions / Time Slice:     Max\n");   /* FIX 10 */
    else
        printf("Instructions / Time Slice:     %d\n",   config->instructions);

    /* Milestone 1: Cache Calculated Values */
    printf("\n***** Cache Calculated Values *****\n\n");
    printf("Total # Blocks:          %d\n",          config->totalBlocks);
    printf("Tag Size:                %d bits (based on actual physical memory)\n", config->tagBits);
    printf("Index Size:              %d bits\n",     config->indexBits);
    printf("Total # Rows:            %d\n",          config->totalRows);
    printf("Overhead Size:           %lld bytes\n",  config->overheadBytes);
    printf("Implementation Memory Size: %.2f KB (%lld bytes)\n",
           config->implementationSizeBytes / 1024.0, config->implementationSizeBytes);
    printf("Cost:                    $%.2f @ $0.07 per KB\n\n", config->cost);

    /* Milestone 1: Physical Memory Calculated Values */
    int physPageBits = (int)log2((double)config->physicalPages);
    printf("\n***** Physical Memory Calculated Values *****\n\n");
    printf("Number of Physical Pages:     %lld\n",  config->physicalPages);
    printf("Number of Pages for System:   %lld ( %.2f * %lld = %lld )\n",
           config->systemPages, config->percentOS/100.0,
           config->physicalPages, config->systemPages);   /* FIX 11 */
    printf("Size of Page Table Entry:     %d bits (1 valid bit, %d for PhysPage)\n",
           config->pageTableEntryBits, physPageBits);
    printf("Total RAM for Page Table(s):  %lld bytes (%dK entries * %d .trc files * %d / 8)\n\n",
           config->totalPageTableBytes, MAX_PTE/1024,
           config->numFiles, config->pageTableEntryBits);

    /* Milestone 2: Virtual Memory Simulation Results */
    long long pagesAvail = config->physicalPages - config->systemPages;
    //int pte_bytes = (config->pageTableEntryBits + 7) / 8;
    printf("\n***** VIRTUAL MEMORY SIMULATION RESULTS *****\n\n");
    printf("Physical Pages Used By SYSTEM: %lld\n", config->systemPages);
    printf("Pages Available to User:       %lld\n\n", pagesAvail);
    printf("Virtual Pages Mapped:          %lld\n", config->virtualPagesMapped);
    printf(" ------------------------------\n");
    printf(" Page Table Hits:   %lld\n", config->pageTableHits);
    printf(" Pages from Free:   %lld\n", config->pagesFromFree);
    printf(" Total Page Faults: %lld\n", config->pageFaults);
    printf("\nPage Table Usage Per Process:\n");
    printf("------------------------------\n\n");
    for (i = 0; i < config->numFiles; i++) {
        double pct = (double)config->usedEntries[i] / MAX_PTE * 100.0;
        long long wasted = (long long)(MAX_PTE - config->usedEntries[i]) * config->pageTableEntryBits / 8; /* FIX 6 */
        printf("[%d] %s:\n", i, config->traceFiles[i]);
        printf("   Used Page Table Entries: %d (%.2f%%)\n", config->usedEntries[i], pct);
        printf("   Page Table Wasted:       %lld bytes\n", wasted);
        printf("\n");
    }

    printf("\n\n");
    
    /* Milestone 3: Cache Simulation Results */
    double hitRate  = (cs->totalCacheAccesses > 0)
        ? cs->cacheHits  * 100.0 / cs->totalCacheAccesses : 0.0;
    double missRate = (cs->totalCacheAccesses > 0)
        ? cs->cacheMisses * 100.0 / cs->totalCacheAccesses : 0.0;
    /* FIX 5: exact CPI from accumulated cycles */
    double cpi = (cs->instrCount > 0) ? cs->totalCycles / cs->instrCount : 0.0;

    long long unusedBlocks = (long long)config->totalBlocks - cs->compulsoryMisses;
    if (unusedBlocks < 0) unusedBlocks = 0;
    double overheadPerBlock = (config->totalBlocks > 0)
        ? (double)config->overheadBytes / config->totalBlocks : 0.0;
    double unusedKB  = (double)unusedBlocks * (config->blockSize + overheadPerBlock) / 1024.0;
    double implKB    = config->implementationSizeBytes / 1024.0;
    double unusedPct = (implKB > 0) ? (unusedKB / implKB) * 100.0 : 0.0;
    double wasteCost = unusedKB * 0.07;
    long long numAddresses = cs->instrCount + cs->srcDstBytes / 4;

    printf("\n***** CACHE SIMULATION RESULTS *****\n\n");
    printf("Total Cache Accesses:  %lld (%lld addresses)\n",
           cs->totalCacheAccesses, numAddresses);
    printf("--- Instruction Bytes: %lld\n", cs->instrBytes);
    printf("--- SrcDst Bytes:      %lld\n", cs->srcDstBytes);
    printf("Cache Hits:            %lld\n", cs->cacheHits);
    printf("Cache Misses:          %lld\n", cs->cacheMisses);
    printf("--- Compulsory Misses: %lld\n", cs->compulsoryMisses);
    printf("--- Conflict Misses:   %lld\n", cs->conflictMisses);
    printf("\n***** ***** CACHE HIT & MISS RATE: ***** *****\n");
    printf("Hit Rate:              %.4f%%\n", hitRate);
    printf("Miss Rate:             %.4f%%\n", missRate);
    printf("CPI:                   %.2f Cycles/Instruction (%lld)\n", cpi, cs->instrCount);
    printf("Unused Cache Space:    %.2f KB / %.2f KB = %.2f%% Waste: $%.2f/chip\n",
           unusedKB, implKB, unusedPct, wasteCost);
    printf("Unused Cache Blocks:   %lld / %d\n", unusedBlocks, config->totalBlocks);
}

/* ---- main ---- */

int main(int argc, char *argv[])
{
    Config config;
    memset(&config, 0, sizeof(config));
    config.instructions = -1;

    parseArguments(argc, argv, &config);
    computeCacheValues(&config);
    computeMemoryValues(&config);
    simulateVirtualMemory(&config);
    printResults(&config);
    return 0;
}