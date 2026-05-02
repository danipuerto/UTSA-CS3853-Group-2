#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
#include<getopt.h>
#include<math.h>
#include<limits.h>

#define MAX_PROCESSES 3  //Maximum number of trace files/processes
#define MAX_PTE 524288   //Max Virtual pages per process
#define PAGE_SIZE 4096U  //Page Size in Bytes
#define LINE_BUFFER 512  //Buffer size for reading trace file lines

//Cache Structure for milestone 3

//One cache way (single cache block slot)
typedef struct{
  int valid;
  unsigned int tag;
  int rr_counter;  //Round Robin Counter (Know which way to evict next)
} CacheBlock;

//One cache row/set
typedef struct {
  CacheBlock *ways;     //Array of associativity blocks
  int rr_next;          //Next way to evict (Round Robin)
}CacheRow;

//MileStone 3 counters
typedef struct {
  long long totalCacheAccesses; //Every row touched
  long long instrBytes;         //Total instrcution bytes read
  long long srcDstBytes;        //Total src/dst bytes read
  long long cacheHits;          //Valid && tag matched
  long long cacheMisses;        //Not valid OR tag mismatch
  long long compulsoryMisses;   //Not Valid (first time loading a block)
  long long conflictMisses;     //Valid but tag mismatch (eviction)
  long long instrCount;         //Number of EIP instruction executed
}CacheStats;

//Contains all simulation parameters and computed results
typedef struct {
    //inputs from user input
    int cacheSizeKB;
    int blockSize;
    int associativity;
    char replacement[5];
    int physicalMemMB;
    float percentOS;
    int instructions;
    int numFiles;
    char traceFiles[3][100];
    
    //Values to be solved for Cache
    int totalBlocks;
    int totalRows;
    int indexBits;
    int offsetBits;
    int tagBits;
    int overheadBytes;
    int implementationSizeBytes;
    double cost;
    
    //Physical memory calculated values
    int physicalPages;
    int systemPages;
    int pageTableEntryBits;
    int totalPageTableBytes;
    
    //Milestone #2 Variables: Virtual memory simulation results
    int pageSystem;
    long long virtualPagesMapped;
    long long pageTableHits;
    long long pagesFromFree;
    long long pageFaults;

    int usedEntries[MAX_PROCESSES];
    long long pageTableWasted[MAX_PROCESSES];
} Config;

//Represents one entry in one processes page table
typedef struct {
    int valid;
    int physical_frame;
} PTE;

//Represents one simulated process (Or one trace file)
typedef struct {
    char *filename;
    PTE *page_table;
    int used_entries;
    int peak_used_entries;
    int finished;
} Process;

//Helps keep Tracj of the current owner of each physical page frame
typedef struct {
    int in_use;
    int owner_pid;
    unsigned int owner_vpn;
} PhysicalPage;

static long long rr_pointer = 0;

/*
    Cache Helper Function (MileStone3)
    Allocate and zero-initialize the cache
*/

CacheRow *createCache(int totalRows, int associativity) {
  int i, j;
  CacheRow *cache = calloc(totalRows, sizeof(CacheBlock));
  for(i = 0; i < totalRows; i++){
    cache[i].ways = calloc(associativity, sizeof(CacheBlocks));
    cache[i].rr_next = 0;
    for(j = 0; j < associativity; j++){
        cache[i].ways[j].valid = 0;
        cache[i].ways[j].tag = 0;
    }
  }
  return cache;
}

//Free the cache
void destroyCache(CacheRow *cache, int totalRows){
    int i;
    for(i = 0; i < totalRows; i++){
        free(cache[i].ways);
    }
    free(cache;);
}

/*
    Access the cache for a single physical address
    
    This function handles One row access. The caller loops over the rows spanned by a multi-byte access
*/
static void accessCacheRow(insigned int rowIndex, unsigned int tag, int associativity, CacheRow *cache, CacheStats *stats, const char *replacement){
  
  int way;
  CacheRow *row = &cache[rowIndex];
  
  stats->totalCacheAccesses+;
  
  //Search all ways for a hit
  for(way = 0; way < associativity; way++){
     if(row->ways[way}.valid && row->ways[way].tag == tag){
      //HIT
      stats->cacheHits++;
      return;
    }
  }
  
  //MISS-find a free way or evict
  stats->cacheMisses++;
  
  //Look for a(n) invalid(empty) way first
  for(way = 0; way < associativity; way++){
    if(!row->ways[way].valid){
      /* Compulsory miss - Block was never loaded */
      stats->compulsoryMisses++;
      rows->ways[way].valid = 1;
      row->ways[way].tag = tag;
      return;
    }
  }
  
  //All ways are valid and no tag matched, which leads to conflict miss (eviction needed)
  stats->conflictMisses++;
  
  //Round robin replacement
  int victim = row->rr_next % associativity;
  row->rr_next = (row->rr_next + 1) % associativity;
  
  row->ways[victim].tag = tag;
  row->ways[victim].valid = 1;
}

/*Process a memory access of numbytes starting at addr
  Splits into per-row access if the access spans a block boundary
*/
static void accessCache(unsigned int addr, int numBytes,int indexBits, int offsetBits, int tagBits, int totalRows, int associativity, CacheRow *cache, CacheStats *stats, const char *replacement) {

  //Ignore zero/invalid addresses
  if(addr == 0)
    return;
    
  unsigned int startBlock = addr >> offsetBits;
  unsigned int endBlock = (addr + numBytes - 1) >> offsetBits;
  
  unsigned int block;
  for(block = startBlock; block <= endBlock; block++){
    unsigned int rowIndex = block & ((1u << indexBits) - 1);
    unsigned int tag = block >> indexBits;
    accessCacheRow(rowIndex, tag, associativity, cache, stats, replacement);
  }
}

/*
    Simulates a virtual to physical adress translation for every one memory access.
*/
void translateAddress(unsigned int addr, int pid,
                      Process *processes, PhysicalPage *phys_pages,
                      long long user_pages, Config *config) {

    /* Extract the virtual page number by shifting off the 12-bit page offset */
    unsigned int vpn = addr >> 12;
    
    if (vpn >= MAX_PTE) return;  /* Skip addresses that exceed the page table size */

    config->virtualPagesMapped++;   /* Count every address lookup as a virtual page mapping attempt */

    if (processes[pid].page_table[vpn].valid) {
        /*
          Case 1: Page Table HIT
          -The page is already mapped to a physical frame
          -No physical memory operation is needed
        */
        config->pageTableHits++;
        
    } else {
        /*
          Case 2: Page Table MISS
          -The page is not currently in physical memory
          -it will first try to find a free/unused physical frame
        */
        
        int found = -1;
        int j;
        for (j = 0; j < user_pages; j++) {
            if (!phys_pages[j].in_use) {
                found = j;  //Space was found within the frame at index j
                break;
            }
        }
        
        if (found != -1) {
        /*
          - SubCase 1: If a Free frame is available
          - Assign the free frame to this virtual page
          - Mark the frame as in use and update the page table entry
        */
            processes[pid].page_table[vpn].valid = 1;
            processes[pid].page_table[vpn].physical_frame = found;

            phys_pages[found].in_use = 1;
            phys_pages[found].owner_pid = pid;
            phys_pages[found].owner_vpn = vpn;

            config->pagesFromFree++; //Loaded from the free list
            
        } else {
            /*
              SubClass 2: If no Frame is found, Activate Round Robin Eviction
              -Case oocures when all frames are occupied, Evict the victim selected by RR
                1. Determin the victim frame index useing the rr_pointer
                2. Invalidate the victims olde page table entry
                3. Map the new virtual page to the now-reclaimed frame
            */
            int victim = rr_pointer % user_pages;
            rr_pointer++;

            //Record which process/page owned the victim frame
            int old_pid = phys_pages[victim].owner_pid;
            int old_vpn = phys_pages[victim].owner_vpn;

            //Invalidate the evicted processes page table netry
            processes[old_pid].page_table[old_vpn].valid = 0;
            processes[old_pid].used_entries--;   //This specific process has lost a mapped page
            
            //Map the new virtual page into the reclaimed frame
            processes[pid].page_table[vpn].valid = 1;
            processes[pid].page_table[vpn].physical_frame = victim;

            //update frame ownership to the new process/page
            phys_pages[victim].owner_pid = pid;
            phys_pages[victim].owner_vpn = vpn;

            config->pageFaults++; //Count this as a page fault (When eviction occures)
        }

        //A new page was mapped for this process(Either from free list or eviction)
        processes[pid].used_entries++;
    }
}

/*
    Combined VM + mileStone 3
    Where the Main virtual memory simulation occurrs (with help of a loop)
    Reads through all trace files round robin, simulating concurrent processes.
    For each instruction slice
     - Reads the EIP (instruction pointer) line and translates the address
     - Reads the following data line and translates dstM / srcM memory operands
     -A process is marked "finished" when it runs out of instructions
     
     After all processes finish, per-process statistics are saved to config.
*/
void simulateVirtualMemory(Config *config) {
    
    /* Initialize process and physical page arrays */
    Process processes[MAX_PROCESSES] = {0};
    
    /* User processes share all frames not reserved for the OS */
    long long user_pages = config->physicalPages - config->systemPages;

    /* Allocate the physical page frame descriptor array */
    PhysicalPage *phys_pages = calloc(user_pages, sizeof(PhysicalPage));

    int i;

    /* Allocate a page table and set the filename for each active process */
    for (i = 0; i < config->numFiles; i++) {
        processes[i].filename = config->traceFiles[i];
        processes[i].page_table = calloc(MAX_PTE, sizeof(PTE));
    }

     /* Explicitly mark all physical frames as free and unowned */
    for (i = 0; i < user_pages; i++) {
        phys_pages[i].in_use = 0;
        phys_pages[i].owner_pid = -1;
    }
    
    //Allocate the cache (Milestone 3)
    CacheRow *cache = creaetCache(config->totalRows, config->associativity);
    CacheStats *cs = &config->cacheStats;
    memset(cs, 0, sizeof(CacheStats));

    char line[LINE_BUFFER];

    /* Open a file handle for each trace file */
    int allDone = 0;
    FILE *fps[MAX_PROCESSES] = {NULL};

    for (i = 0; i < config->numFiles; i++) {
        fps[i] = fopen(config->traceFiles[i], "r");
        if (!fps[i]) processes[i].finished = 1;
    }

    /*
      Main Simulation Loop
      * Keep going as long as at least one process still has instructions.
      * Each outer iteration is one "round" through all active processes. */
      
    while (!allDone) {
        allDone = 1;   /* Assume done; set to 0 if any process is still active */

        for (i = 0; i < config->numFiles; i++) {
            if (processes[i].finished) continue;  /* Skip completed processes */
            allDone = 0;     /* At least one process is still active */

            int instrCount = 0;
            
            /* Determine how many instructions to process this time slice */
            int limit = (config->instructions == -1) ? INT_MAX : config->instructions;

            /* Read up to `limit` instructions from this process's trace file */
            while (instrCount < limit) {
                int gotEIP = 0;
                
                /* 
                  Scan for the next EIP (Instruction fetch) line 
                  - Lines look like "EIP (02): 010c1517" */
                while (fgets(line, sizeof(line), fps[i])) {
                    if (strncmp(line, "EIP", 3) == 0) {
                        gotEIP = 1;
                        break;
                    }
                }

                /* No more EIP lines, this means this process is finished*/
                if (!gotEIP) {
                    fclose(fps[i]);
                    fps[i] = NULL;
                    processes[i].finished = 1;
                    break;
                }

                /* Parse the instruction address from the EIP line */
                unsigned int addr;
                int len;
                if (sscanf(line, "EIP (%d): %x", &len, &addr) == 2) {
                  /* Translate the instruction fetch address */
                    translateAddress(addr, i, processes, phys_pages, user_pages, config);
                    
                    //Cache access for instuction fetch 
                    accessCache(addr, len, config->indexBits, config->offsetBits, config->tagBits, config->totalRows, config->associativity, cache, cs config->replacement);
                    
                    cs->instrBytes += len;
                    cs->instrCount++;
                }

                /*
                     Read the memory operand line (follows each EIP line)
                     We translate dstM and srcM addresses if they are non-zero
                     and not a dash ('-'), which indicates no memory access. 
                     - Lines look like: "dstM: 00000000 --------    srcM: 00000000 --------   "*/
                if (fgets(line, sizeof(line), fps[i])) {
                    char *dstPtr = strstr(line, "dstM:");
                    char *srcPtr = strstr(line, "srcM:");

                    /* Translate destination memory address (write) */
                    if (dstPtr) {
                        unsigned int dstAddr = 0;
                        char dstData[16] = {0};
                        sscanf(dstPtr + 6, "%x %15s", &dstAddr, dstData);
                        /* Only translate if address is non-zero and not '-' */
                        if (dstAddr != 0 && dstData[0] != '-')
                            translateAddress(dstAddr, i, processes, phys_pages, user_pages, config);
                    }

                    /* Translate source memory address (read)*/
                    if (srcPtr) {
                        unsigned int srcAddr = 0;
                        char srcData[16] = {0};
                        sscanf(srcPtr + 6, "%x %15s", &srcAddr, srcData);
                        /* Only translate if address is non-zero and not '-' */
                        if (srcAddr != 0 && srcData[0] != '-'){
                            translateAddress(srcAddr, i, processes, phys_pages, user_pages, config);
                            
                            //4 byte data access
                            accessCache(srcAddr, 4, configindexBits, config->offsetBits, config->tagBits, config->totalRows, config->associativity, cache, cs, config->replacement);
                            cs->srcDstBytes += 4;
                       }
                    }
                }

                instrCount++;   /* One full instruction (EIP + memory operands) processed */
            }
        }
    }
    
    destroyCache(cache, config->totalRows);

     /* Wasted bytes = (unused entries) * (bits per PTE) / 8
     * Unused entries are those that were never mapped during simulation. */
    for (i = 0; i < config->numFiles; i++) {
        config->usedEntries[i] = processes[i].used_entries;

        config->pageTableWasted[i] =
            (long long)(MAX_PTE - processes[i].used_entries)
            * config->pageTableEntryBits / 8;

        if (processes[i].page_table) free(processes[i].page_table);
    }

    free(phys_pages);
}

/*
    Parses/helps with input values when types from the user. Also helps fill/populate
    the Config struct 
*/
void parseArguments(int argc, char *argv[], Config *config){
  
  int i;
  for(i = 1; i < argc; i++){
    
    if(strcmp(argv[i], "-s") == 0){
      config->cacheSizeKB = atoi(argv[++i]);
      
      if(config->cacheSizeKB < 8 || config->cacheSizeKB > 8192){
          printf("Invalid Cache Size\n");
          exit(1);
      }
      
    }
      
    else if(strcmp(argv[i], "-b") == 0){
      config->blockSize = atoi(argv[++i]);
      
       if(config->blockSize < 8 || config->blockSize > 64){
          printf("Invalid Block Size\n");
          exit(1);
      }
    }
      
    else if(strcmp(argv[i], "-a") == 0){
      config->associativity = atoi(argv[++i]);
    
     if(config->associativity != 1 && config->associativity != 2 && config->associativity != 4 && config->associativity != 8 && config->associativity != 16){
          printf("Invalid Associativity Size\n");
          exit(1);
      }  
    }
      
    else if(strcmp(argv[i], "-r") == 0){
      strcpy(config->replacement, argv[++i]);
      
      if (strcmp(config->replacement, "RR") != 0 && strcmp(config->replacement, "rr") != 0 && strcmp(config->replacement, "RND") != 0 && strcmp(config->replacement, "rnd") != 0)  {
          printf("Invalid Replacement\n");
          exit(1);
      }
      
    }
      
    else if(strcmp(argv[i], "-p") == 0){
      config->physicalMemMB = atoi(argv[++i]);
      
       if(config->physicalMemMB < 128 || config->physicalMemMB > 4096){
          printf("Invalid Physical Memory\n");
          exit(1);
      }
      
    }
      
    else if(strcmp(argv[i], "-u") == 0){
      config->percentOS = atof(argv[++i]);
      
      if (config->percentOS < 0 || config->percentOS > 100) {
        printf("Invalid OS memory percentage\n");
        exit(1);
      }
      
    }
      
    else if(strcmp(argv[i], "-n") == 0){
      config->instructions = atoi(argv[++i]);
      
      if(!(config->instructions == -1 || config->instructions >= 1)){
          printf("Invalid Instruction\n");
          exit(1);
      }
    }
    
    else if(strcmp(argv[i], "-f") == 0) {
      if(config->numFiles < 3){
          strcpy(config->traceFiles[config->numFiles], argv[++i]);
          config->numFiles++;
      }
      else{
        printf("Exceed file limit\n");
        exit(1);
      }
      
    }
            
  }
}
/*
    Calculated all cache related values based from the users input. Below are
    some of the formulas used for calculating each variable.
    
    totalBlocks    = cacheBytes / blockSize
    totalRows      = cacheBytes / (associativity * blockSize)
    offsetBits     = log2(blockSize)
    indexBits      = log2(totalRows)
    tagBits        = physicalAddressBits - indexBits - offsetBits
    overheadBytes  = totalBlocks * (tagBits + 1 valid bit) / 8
    implSizeBytes  = cacheBytes + overheadBytes
    cost           = implSizeBytes (in KB) * $0.07
*/

void computeCacheValues(Config *config){

   long long cacheBytes = (long long)config->cacheSizeKB * 1024;
   
   //Total blocks in cache and number rows/sets
   config->totalBlocks = cacheBytes / config->blockSize;
   config->totalRows = cacheBytes / (config->associativity * config->blockSize);

   //Address bit breakdown
   config->offsetBits = (int)log2(config->blockSize);
   config->indexBits = (int)log2(config->totalRows);

   //Physical address width based from the physical memory size
   long long physicalBytes = (long long)config->physicalMemMB * 1024 * 1024;
   int physicalBits = (int)log2(physicalBytes);

    //Tag bits
   config->tagBits = physicalBits - config->indexBits - config->offsetBits;

   //Overhead
   config->overheadBytes = (config->totalBlocks * (config->tagBits + 1)) / 8;

   //Total Implementation size, which includes the data array and overhead
   config->implementationSizeBytes = cacheBytes + config->overheadBytes;
 
   //Cost when at $0.07 per KB
   config->cost = (config->implementationSizeBytes / 1024.0)  * 0.07;
}

/*
    Computes and Derives all Physical memory based on the inputs that
    the user entered. Below will be the formulas that use used for 
    each variable.
    
    physicalPages      = physicalBytes / PAGE_SIZE (4096)
    systemPages        = physicalPages * (percentOS / 100)
    pageTableEntryBits = 1 (valid) + log2(physicalPages) (frame number bits)
    totalPageTableBytes= (512K entries * numFiles * PTE bits) / 8
    
*/

void computeMemoryValues(Config *config){

  long long physicalByte = (long long)config->physicalMemMB * 1024 * 1024;
  
  //Total number of physical page frames
  config->physicalPages = physicalByte / 4096;

  //Number of frames reserved for the OS
  config->systemPages = config->physicalPages * (config->percentOS / 100.00);
  
  //Bits needed for the PTE
  config->pageTableEntryBits = 1 + (int)log2(config->physicalPages);
  
  //Total amount of RAM consumed by all page tables
  config->totalPageTableBytes = ((512 * 1024) * config->numFiles * config->pageTableEntryBits) / 8;
  
  config->pageSystem = (int)(config->percentOS / 100.0 * config->physicalPages);
}

/*
    This Function Prints all simulation results to the stdout, organized based
    on what was provided within the Virtual Memory & Cache Simulator pdf on
    Canvas. The print is organized as listed below
     1. Trace file list
     2. Cache input parameters
     3. Cache calculated values
     4. Physical memory calculated values
     5. Virtual memory simulation results (Milestone 2)

*/
void printResults(Config *config){
 
  int i;
  printf("Cache Simulator - CS 3853 - Team #2\n\n");
  
  //Trace file print input
  printf("Trace File(s):\n\n");
  for(i = 0; i < config->numFiles; i++){
    printf("        %s\n", config->traceFiles[i]);
  }
  
  //Cache input Parameters
  printf("\n\n***** Cache Input Parameters *****\n\n");
  printf("Cache Size:                     %d KB\n", config->cacheSizeKB);
  printf("Block Size:                     %d bytes\n", config->blockSize);
  printf("Associativity:                  %d\n", config->associativity);
  
  //Prints whether user chooses between Round Robin or Random
   if (strcmp(config->replacement, "RR") == 0 || strcmp(config->replacement, "rr") == 0)
        printf("Replacement Policy:             Round Robin\n");
   else
        printf("Replacement Policy:             Random\n");
   
   printf("Physical Memory:                %d MB\n", config->physicalMemMB);
   printf("Percent Memory Used by System:  %.1f%%\n", config->percentOS);
   
   if(config->instructions == -1)
     printf("Instructions / Time Slice:      ALL\n");
   else
     printf("Instructions / Time Slice:      %d\n", config->instructions);
   
  //Cache Calculated Values
   printf("\n\n***** Cache Calculated Values *****\n\n");
   printf("Total # Blocks:                 %d\n", config->totalBlocks);
   printf("Tag Size:                       %d bits\n", config->tagBits);
   printf("Index Size:                     %d bits\n", config->indexBits);
   printf("Total # Rows:                   %d\n", config->totalRows);
   printf("Overhead Size:                  %d bytes\n", config->overheadBytes);
   printf("Implementation Memory Size:     %.2f KB (%d bytes)\n", config->implementationSizeBytes / 1024.0,config->implementationSizeBytes);
   printf("Cost:                           $%.2f @ $0.07 per KB\n", config->cost);
  
  //Physical Memory Calculated Values
   printf("\n\n***** Physical Memory Calculated Values *****\n\n");
   printf("Number of Physical Pages:       %d\n", config->physicalPages);
   printf("Number of Pages for System:     %d\n", config->systemPages);
   printf("Size of Page Table Entry:       %d bits\n", config->pageTableEntryBits);
   printf("TotalF RAM for Page Table(s):    %d bytes\n", config->totalPageTableBytes);
  
  //Virtual Memory Simulation Results
   printf("\n\n***** Virtual Memory Simulation Results *****\n\n");
   printf("Physical Pages Used By SYSTEM:  %d\n", config->pageSystem);
   printf("Pages Available to User:        %d\n\n", config->physicalPages - config->pageSystem);
   printf("Virtual Pages Mapped:           %lld\n", config->virtualPagesMapped);
   printf("        ------------------------------\n");
   printf("        Page Table Hits:        %lld\n\n", config->pageTableHits);
   printf("        Pages from Free:        %lld\n\n", config->pagesFromFree);
   printf("        Total Page Faults:      %lld\n\n\n", config->pageFaults);

  //Per-Process Page Table Usage
   printf("Page Table Usage Per Process:\n");
   printf("------------------------------\n");
   for (i = 0; i < config->numFiles; i++) {
       //Percentage of the Cache Size-entry table that was actually used
       double pct = ((double)config->usedEntries[i] / MAX_PTE) * 100.0;
       printf("[%d] %s:\n", i, config->traceFiles[i]);
       printf("        Used Page Table Entries: %d ( %.2f%%)\n", 
              config->usedEntries[i], pct);
       printf("        Page Table Wasted:       %lld bytes\n", 
              config->pageTableWasted[i]);
       printf("\n");
   }
   
   // MileStone 3: Cache Simulation Results
   
   //Derived stats
   double hitRate =  (cs->totalCacheAccesses > 0) ? (cs->cacheHits  * 100.0) / cs->totalCacheAccesses : 0.0;
   
    double missRate = (cs->totalCacheAccesses > 0) ? (cs->cacheMisses * 100.0) / cs->totalCacheAccesses : 0.0;
    
    /*CPI formulas to keep note of
      
      totalCycles = instrCount + cacheMisses * miss_penalty
      CPI = 1 + (miss_rate × miss_penalty)
    
    */
    long long = MissPenalty = 22;
    
     double totalCycles = (cs->instrCount > 0)
                         ? (double)(cs->instrCount + cs->cacheMisses * missPenalty)
                         : 0.0;
    double cpi = (cs->instrCount > 0) ? totalCycles / cs->instrCount : 0.0;


    long long unusedBlocks = config->totalBlocks - cs->compulsoryMisses;
    
    double overheadPerBlock = (config->totalBlocks > 0) ? (double)config->overheadBytes / config->totalBlocks : 0.0;
    
    double unusedKB  = unusedBlocks * (config->blockSize + overheadPerBlock) / 1024.0;
    double implKB    = config->implementationSizeBytes / 1024.0;
    double unusedPct = (implKB > 0) ? (unusedKB / implKB) * 100.0 : 0.0;
    double wasteCost = unusedKB * 0.07;

    printf("***** CACHE SIMULATION RESULTS *****\n\n");
    printf("Total Cache Accesses:           %lld     (%lld addresses)\n",
           cs->totalCacheAccesses, config->virtualPagesMapped);
    printf("--- Instruction Bytes:          %lld\n", cs->instrBytes);
    printf("--- SrcDst Bytes:               %lld\n", cs->srcDstBytes);
    printf("Cache Hits:                     %lld\n", cs->cacheHits);
    printf("Cache Misses:                   %lld\n", cs->cacheMisses);
    printf("--- Compulsory Misses:          %lld\n", cs->compulsoryMisses);
    printf("--- Conflict Misses:            %lld\n\n", cs->conflictMisses);

    printf("***** *****  CACHE HIT & MISS RATE:  ***** *****\n\n");
    printf("Hit Rate:                       %.4f%%\n", hitRate);
    printf("Miss Rate:                      %.4f%%\n", missRate);
    printf("CPI:                            %.2f Cycles/Instruction  (%lld)\n",
           cpi, cs->instrCount);
    printf("Unused Cache Space:             %.2f KB / %.2f KB = %.2f%%  Waste: $%.2f/chip\n",
           unusedKB, implKB, unusedPct, wasteCost);
    printf("Unused Cache Blocks:            %lld / %d\n",
           unusedBlocks, config->totalBlocks);

}



int main(int argc, char *argv[]){
  Config config = {0}; // Zero-Initialization for all fields within Config
  
  parseArguments(argc, argv, &config);
  computeCacheValues(&config);
  computeMemoryValues(&config);
  simulateVirtualMemory(&config);
  printResults(&config);
  
  return 0;

}

