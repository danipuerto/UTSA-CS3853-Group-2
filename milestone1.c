#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<math.h>

typedef struct {
    //inputs from user input
    int cacheSizeKB;
    int blockSize;
    int associativity;
    char replacement[4];
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
    
} Config;


void parseArguments(int argc, char *argv[], Config *config){
  
  int i;
  for(i = 1; i < argc; i++){
    
    if(strcmp(argv[i], "-s") == 0)
      config->cacheSizeKB = atoi(argv[++i]);
      
    else if(strcmp(argv[i], "-b") == 0)
      config->blockSize = atoi(argv[++i]);
      
    else if(strcmp(argv[i], "-a") == 0)
      config->associativity = atoi(argv[++i]);
      
    else if(strcmp(argv[i], "-r") == 0)
      strcpy(config->replacement, argv[++i]);
      
    else if(strcmp(argv[i], "-p") == 0)
      config->physicalMemMB = atoi(argv[++i]);
      
    else if(strcmp(argv[i], "-u") == 0){
      config->percentOS = atof(argv[++i]);
      
      if (config->percentOS < 0 || config->percentOS > 100) {
        printf("Invalid OS memory percentage\n");
        exit(1);
      }
      
    }
      
    else if(strcmp(argv[i], "-n") == 0)
      config->instructions = atoi(argv[++i]);
    
    else if(strcmp(argv[i], "-f") == 0) {
      if(config->numFiles < 3){
          strcpy(config->traceFiles[config->numFiles], argv[++i]);
          config->numFiles++;
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

    printf("Number of Pages for System:     %d\n", config->systemPages); //(0.75 * 262144 = 196608)

    printf("Size of Page Table Entry:       %d bits\n", config->pageTableEntryBits);  //(1 valid bit, 18 for PhysPage)

    printf("Total RAM for Page Table(s):    %d bytes\n", config->totalPageTableBytes);  //(512K entries * 3 .trc files * 19 / 8)
  
}



int main(int argc, char *argv[]){
  Config config = {0}; //Set values to 0
  
  parseArguments(argc, argv, &config);
  computeCacheValues(&config);
  computeMemoryValues(&config);
  printResults(&config);
  
  return 0;

}
