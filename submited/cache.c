#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#include "cache.h"

static cache_entry_t *cache = NULL;
static int cache_size = 0;
static int clock = 0;
static int num_queries = 0;
static int num_hits = 0;
bool cache_created = false;

int cache_create(int num_entries) {
  if (num_entries < 2 || num_entries > 4096){
    return -1;
  }
  if (cache_created == true){
    return -1;
  }
  cache_size = num_entries;
  cache = calloc(cache_size, sizeof(cache_entry_t));
  cache_created = true;
  return 1;
}

int cache_destroy(void) {
  if(cache_created == false){
    return -1;
  }
  free(cache);
  cache_size = 0;
  cache = NULL;
  cache_created = false;
  return 1;
}

//inputs disk_num, block_num are treated as keys of the struct cache_entry_t
//input buf is the value of the struct cache_entry_t
//when function is called:
//increment the num_queries and clock variable by 1, and copy the clock variable into the cache access time
//loop through the cache items, looking for a matching pair of disk_num and block_num
//if there is a match, copy the struct block into the input buf

int cache_lookup(int disk_num, int block_num, uint8_t *buf) {
  if (buf == NULL){
    return -1;
  }
  if (cache_created == false){
    return -1;
  }

  clock += 1;
  num_queries += 1;

  for (int i = 0; cache + i < cache + cache_size; i++){
    if((cache + i)->valid && (cache + i)->disk_num == disk_num && (cache + i)->block_num == block_num){
      //cache hit!
      memcpy(buf,(cache + i)->block,JBOD_BLOCK_SIZE);
      num_hits += 1;
      (cache + i)->access_time = clock;
      return 1;
    }
  }
  return -1;
}

//looks for the item in the cache and updates its value if it exists
void cache_update(int disk_num, int block_num, const uint8_t *buf) {
  for (int i = 0; cache + i < cache + cache_size; i++){
    if((cache + i)->valid && (cache + i)->disk_num == disk_num && (cache + i)->block_num == block_num){
      memcpy((cache + i)->block, buf, JBOD_BLOCK_SIZE);
      (cache + i)->access_time = clock;
      return;
    }
  }
  return;
}



//find an invalid cache item, or ejects an item. Place the inputted data into the empty cache spot.
int cache_insert(int disk_num, int block_num, const uint8_t *buf) {
  if(buf == NULL){
    return -1;
  }
  if(cache_created == false){
    return -1;
  }
  if(disk_num < 0 || disk_num >= JBOD_NUM_DISKS){
    return -1;
  }
  if(block_num < 0 || block_num >= JBOD_NUM_BLOCKS_PER_DISK){
    return -1;
  }
  
  clock += 1;

  cache_entry_t *evict_cache = NULL; //stores the location of an invalid or a lru cache item
  int lru_access_time = INT32_MAX;

  for (int i = 0; cache + i < cache + cache_size; i++){
    //check if item already exists
    if((cache + i)->valid && (cache + i)->disk_num == disk_num && (cache + i)->block_num == block_num){
      //cache item already exists!
      //update it, I will just update the block in this function without calling update, to optimize the cache
      memcpy((cache + i)->block, buf, JBOD_BLOCK_SIZE);
      (cache + i)->access_time = clock;
      return -1;
    }
    //check for invalid cache items
    if((cache + i)->valid == false){
      evict_cache = (cache + i);
      break;
    }
    //check for least recently used cache item
    if (lru_access_time > (cache + i)->access_time){
      lru_access_time = (cache + i)->access_time;
      evict_cache = (cache + i);
    }
  }

  //replace the invalid or the lru cache item with the new cache block
  evict_cache->valid = true;
  evict_cache->disk_num = disk_num;
  evict_cache->block_num = block_num;
  memcpy(evict_cache->block, buf, JBOD_BLOCK_SIZE);
  evict_cache->access_time = clock;

  return 1;
}

bool cache_enabled(void) {
  return cache_created;
}

void cache_print_hit_rate(void) {
  fprintf(stderr, "Hit rate: %5.1f%%\n", 100 * (float) num_hits / num_queries);
}
