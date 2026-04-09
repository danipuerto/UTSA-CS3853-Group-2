#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
#include<getopt.h>
#include<math.h>
#include<limits.h>

#define MAX_PROCESSES 3
#define MAX_PTE 524288
#define PAGE_SIZE 4096U
#define LINE_BUFFER 512

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
    
    //Values to be solved
    int totalBlocks;
    int totalRows;
    int indexBits;
    int offsetBits;
    int tagBits;
    int overheadBytes;
    int implementationSizeBytes;
    double cost;
    
    int physicalPages;
    int systemPages;
    int pageTableEntryBits;
    int totalPageTableBytes;
    
    //Milestone #2 Variables
    int pageSystem;
    long long virtualPagesMapped;
    long long pageTableHits;
    long long pagesFromFree;
    long long pageFaults;

    int usedEntries[MAX_PROCESSES];
    long long pageTableWasted[MAX_PROCESSES];
} Config;

typedef struct {
    int valid;
    int physical_frame;
} PTE;

typedef struct {
    char *filename;
    PTE *page_table;
    int used_entries;
    int peak_used_entries;
    int finished;
} Process;

typedef struct {
    int in_use;
    int owner_pid;
    unsigned int owner_vpn;
} PhysicalPage;

static long long rr_pointer = 0;

void translateAddress(unsigned int addr, int pid,
                      Process *processes, PhysicalPage *phys_pages,
                      long long user_pages, Config *config) {

    unsigned int vpn = addr >> 12;
    if (vpn >= MAX_PTE) return;

    config->virtualPagesMapped++;   

    if (processes[pid].page_table[vpn].valid) {
        config->pageTableHits++;
    } else {
        int found = -1;
        int j;
        for (j = 0; j < user_pages; j++) {
            if (!phys_pages[j].in_use) {
                found = j;
                break;
            }
        }

        if (found != -1) {
            processes[pid].page_table[vpn].valid = 1;
            processes[pid].page_table[vpn].physical_frame = found;

            phys_pages[found].in_use = 1;
            phys_pages[found].owner_pid = pid;
            phys_pages[found].owner_vpn = vpn;

            config->pagesFromFree++;
        } else {
            int victim = rr_pointer % user_pages;
            rr_pointer++;

            int old_pid = phys_pages[victim].owner_pid;
            int old_vpn = phys_pages[victim].owner_vpn;

            processes[old_pid].page_table[old_vpn].valid = 0;
            processes[old_pid].used_entries--;  
            processes[pid].page_table[vpn].valid = 1;
            processes[pid].page_table[vpn].physical_frame = victim;

            phys_pages[victim].owner_pid = pid;
            phys_pages[victim].owner_vpn = vpn;

            config->pageFaults++;
        }

        processes[pid].used_entries++;
    }
}

void simulateVirtualMemory(Config *config) {

    Process processes[MAX_PROCESSES] = {0};
    long long user_pages = config->physicalPages - config->systemPages;

    PhysicalPage *phys_pages = calloc(user_pages, sizeof(PhysicalPage));

    int i;

    // Initialize processes
    for (i = 0; i < config->numFiles; i++) {
        processes[i].filename = config->traceFiles[i];
        processes[i].page_table = calloc(MAX_PTE, sizeof(PTE));
    }

    // Initialize physical pages
    for (i = 0; i < user_pages; i++) {
        phys_pages[i].in_use = 0;
        phys_pages[i].owner_pid = -1;
    }

    char line[LINE_BUFFER];

    int allDone = 0;
    FILE *fps[MAX_PROCESSES] = {NULL};

    for (i = 0; i < config->numFiles; i++) {
        fps[i] = fopen(config->traceFiles[i], "r");
        if (!fps[i]) processes[i].finished = 1;
    }

    while (!allDone) {
        allDone = 1;

        for (i = 0; i < config->numFiles; i++) {
            if (processes[i].finished) continue;
            allDone = 0;

            int instrCount = 0;
            int limit = (config->instructions == -1) ? INT_MAX : config->instructions;

            while (instrCount < limit) {
                int gotEIP = 0;
                while (fgets(line, sizeof(line), fps[i])) {
                    if (strncmp(line, "EIP", 3) == 0) {
                        gotEIP = 1;
                        break;
                    }
                }

                if (!gotEIP) {
                    fclose(fps[i]);
                    fps[i] = NULL;
                    processes[i].finished = 1;
                    break;
                }

                unsigned int addr;
                int len;
                if (sscanf(line, "EIP (%d): %x", &len, &addr) == 2) {
                    translateAddress(addr, i, processes, phys_pages, user_pages, config);
                }

                if (fgets(line, sizeof(line), fps[i])) {
                    char *dstPtr = strstr(line, "dstM:");
                    char *srcPtr = strstr(line, "srcM:");

                    if (dstPtr) {
                        unsigned int dstAddr = 0;
                        char dstData[16] = {0};
                        sscanf(dstPtr + 6, "%x %15s", &dstAddr, dstData);
                        if (dstAddr != 0 && dstData[0] != '-')
                            translateAddress(dstAddr, i, processes, phys_pages, user_pages, config);
                    }

                    if (srcPtr) {
                        unsigned int srcAddr = 0;
                        char srcData[16] = {0};
                        sscanf(srcPtr + 6, "%x %15s", &srcAddr, srcData);
                        if (srcAddr != 0 && srcData[0] != '-')
                            translateAddress(srcAddr, i, processes, phys_pages, user_pages, config);
                    }
                }

                instrCount++;
            }
        }
    }

    // Save per-process stats
    for (i = 0; i < config->numFiles; i++) {
        config->usedEntries[i] = processes[i].used_entries;

        config->pageTableWasted[i] =
            (long long)(MAX_PTE - processes[i].used_entries)
            * config->pageTableEntryBits / 8;

        if (processes[i].page_table) free(processes[i].page_table);
    }

    free(phys_pages);
}

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

void computeCacheValues(Config *config){

   long long cacheBytes = (long long)config->cacheSizeKB * 1024;

   config->totalBlocks = cacheBytes / config->blockSize;
   config->totalRows = cacheBytes / (config->associativity * config->blockSize);

   config->offsetBits = (int)log2(config->blockSize);
   config->indexBits = (int)log2(config->totalRows);

   long long physicalBytes = (long long)config->physicalMemMB * 1024 * 1024;
   int physicalBits = (int)log2(physicalBytes);

   config->tagBits = physicalBits - config->indexBits - config->offsetBits;

  config->overheadBytes = (config->totalBlocks * (config->tagBits + 1)) / 8;

  config->implementationSizeBytes = cacheBytes + config->overheadBytes;
 
  config->cost = (config->implementationSizeBytes / 1024.0)  * 0.07;
}

void computeMemoryValues(Config *config){

  long long physicalByte = (long long)config->physicalMemMB * 1024 * 1024;
  
  config->physicalPages = physicalByte / 4096;

  config->systemPages = config->physicalPages * (config->percentOS / 100.00);
  
  config->pageTableEntryBits = 1 + (int)log2(config->physicalPages);
  
  config->totalPageTableBytes = ((512 * 1024) * config->numFiles * config->pageTableEntryBits) / 8;
  
  config->pageSystem = (int)(config->percentOS / 100.0 * config->physicalPages);
  
  
}

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
  
  //Choice of either round robin or random
   if (strcmp(config->replacement, "RR") == 0 || strcmp(config->replacement, "rr") == 0)
        printf("Replacement Policy:             Round Robin\n");
   else
        printf("Replacement Policy:             Random\n");
   
   printf("Physical Memory:                %d MB\n", config->physicalMemMB);
   printf("Percent Memory Used by System:  %.1f%%\n", config->percentOS);
   if(config->instructions == -1)
     printf("Instructions / Time Slice:      MAX\n");
   else
     printf("Instructions / Time Slice:      %d\n", config->instructions);
   
  //Cache values Calculated
   printf("\n\n***** Cache Calculated Values *****\n\n");

    printf("Total # Blocks:                 %d\n", config->totalBlocks);
    printf("Tag Size:                       %d bits\n", config->tagBits);
    printf("Index Size:                     %d bits\n", config->indexBits);
    printf("Total # Rows:                   %d\n", config->totalRows);
    printf("Overhead Size:                  %d bytes\n", config->overheadBytes);

    printf("Implementation Memory Size:     %.2f KB (%d bytes)\n", config->implementationSizeBytes / 1024.0,config->implementationSizeBytes);

    printf("Cost:                           $%.2f @ $0.07 per KB\n", config->cost);
  
  //Physical Memory Calculated values
   printf("\n\n***** Physical Memory Calculated Values *****\n\n");

    printf("Number of Physical Pages:       %d\n", config->physicalPages);

    printf("Number of Pages for System:     %d\n", config->systemPages);

    printf("Size of Page Table Entry:       %d bits\n", config->pageTableEntryBits);

    printf("Total RAM for Page Table(s):    %d bytes\n", config->totalPageTableBytes);
  
  //Virtual Memory Simulation Results
   printf("\n\n***** Virtual Memory Simulation Results *****\n\n");
   printf("Physical Pages Used By SYSTEM:  %d\n", config->pageSystem);
   printf("Pages Available to User:        %d\n\n", config->physicalPages - config->pageSystem);
   printf("Virtual Pages Mapped:           %lld\n", config->virtualPagesMapped);
   printf("        ------------------------------\n");
   printf("        Page Table Hits:        %lld\n\n", config->pageTableHits);
   printf("        Pages from Free:        %lld\n\n", config->pagesFromFree);
   printf("        Total Page Faults:      %lld\n\n\n", config->pageFaults);

   printf("Page Table Usage Per Process:\n");
   printf("------------------------------\n");
   for (i = 0; i < config->numFiles; i++) {
       double pct = ((double)config->usedEntries[i] / MAX_PTE) * 100.0;
       printf("[%d] %s:\n", i, config->traceFiles[i]);
       printf("        Used Page Table Entries: %d ( %.2f%%)\n", 
              config->usedEntries[i], pct);
       printf("        Page Table Wasted:       %lld bytes\n", 
              config->pageTableWasted[i]);
       printf("\n");
   }
}



int main(int argc, char *argv[]){
  Config config = {0}; //Set values to 0
  
  parseArguments(argc, argv, &config);
  computeCacheValues(&config);
  computeMemoryValues(&config);
  simulateVirtualMemory(&config);
  printResults(&config);
  
  return 0;

}
