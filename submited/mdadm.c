#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "mdadm.h"
#include "jbod.h"
#include "net.h"

// -1 means unmounted, 1 means mounted
int mounted = -1;

int encode_op(uint32_t command, uint32_t diskID, uint32_t blockID){
  uint32_t op = 0;
  op |= (command << 26);
  op |= (diskID << 22);
  op |= blockID;
  //printf("\nop is %x\n", op);
  return op;
}

int mdadm_mount(void) {
  if (mounted == 1){ //if mounted, can not mount again
    return -1;
  }
  uint32_t op = encode_op(JBOD_MOUNT, 0, 0);
  int rc = jbod_client_operation(op, NULL);
  if (rc == 0){
    rc = 1;
    mounted = 1;
  }
  return rc;
}

int mdadm_unmount(void) {
  if (mounted == -1){ //if not mounted, can not unmount again
    return -1;
  }
  uint32_t op = encode_op(JBOD_UNMOUNT, 0, 0);
  int rc = jbod_client_operation(op, NULL);
  if (rc == 0){
    rc = 1;
    mounted = -1;
  }
  return rc;
}

int min(int x, int y){
  if(x <= y){
    return x;
  }
  else{
    return y;
  }
  return -1;
}

int mdadm_read(uint32_t addr, uint32_t len, uint8_t *buf) {
  if (mounted == -1){ //can not read from an unmounted JBOD
    return -1;
  }
  if (len > 1024){ //len is greater than buffer
    return -1;
  }
  if (addr >= 1048576){ //trying to read from an address larger than the size of the bytes
    return -1;
  }
  if ((addr + len) > 1048576){ //trying to read out of bounds
    return -1;
  }
  if (len > 0 && buf == NULL){
    return -1;
  }
  if (len == 0 && buf == NULL){
    return 0;
  }

  //flag to check cache enabled
  bool cache_created = cache_enabled();
  //flag to see if cache lookup succeeded
  int cache_block_located = -1;
  
  int num_read = 0; //bytes that will have been read into mybuf
  int checklen = len; //return an unmodified len

  //mybuf will contain each read call's bytes
  uint8_t mybuf[256];

  //encode the read block operation
  uint32_t opRead = encode_op(JBOD_READ_BLOCK,0,0);
  
  while (len > 0){
    //now we need to find disk id, block id and find out the offset from the start of a block.
    uint32_t diskID = addr/65536;
    uint32_t blockID = (addr%65536)/256;
    uint32_t offset = addr%256;

    //assume there the cache does not have the desired cache block by default.
    cache_block_located = -1;

    //if the cache is enabled, we need to look in the cache for block and load the block into mybuf
    if (cache_created){
      cache_block_located = cache_lookup(diskID, blockID, mybuf);
    }

    if (cache_block_located == -1){ //either no cache exists, or the block is not found, so we must find the block in JBOD
      //jbod operation will now be set to the correct block and disk
      uint32_t opDisk = encode_op(JBOD_SEEK_TO_DISK,diskID,0);
      uint32_t opBlock = encode_op(JBOD_SEEK_TO_BLOCK, 0, blockID);

      //call jbod to set to block and disk
      jbod_client_operation(opDisk,NULL);
      jbod_client_operation(opBlock,NULL);

      //now call jbod read operation to read bytes into mybuf
      jbod_client_operation(opRead, mybuf);

      //if a cache exists, we must update the cache to insert this block
      if (cache_created){
        cache_insert(diskID, blockID, mybuf);
      }
    }

    //mybuf now has a block from JBOD stored

    //we either need the bytes in the whole block, bytes from the end of the block, or a few bytes within a block
    int num_bytes_to_read_from_block = min(len, min(256, 256-offset));

    //we use memcpy to copy the nunber of bytes in my buffer to the main buffer
    memcpy(buf + num_read, mybuf + offset, num_bytes_to_read_from_block);
    //buf + num_read keeps track of the index in buf that holds the last copied byte.
    //mybuf + offset tracks the position of bytes that will be copied
    //num_bytes_to_read_from_block tracks the size of the copy

    num_read += num_bytes_to_read_from_block; //we have read more bytes after each iteration
    len -= num_bytes_to_read_from_block; //the number of bytes that need to be copied decreases after each iteration
    addr += num_bytes_to_read_from_block; //addr points to next block or the end of the block
  }
  return checklen;
}

int mdadm_write(uint32_t addr, uint32_t len, const uint8_t *buf) {
    if (mounted == -1){ //can not write to an unmounted JBOD
    return -1;
  }
  if (len > 1024){ //len is greater than buffer
    return -1;
  }
  if (addr >= 1048576){ //trying to write to an address larger than the size of the JBOD
    return -1;
  }
  if ((addr + len) > 1048576){ //trying to write out of bounds
    return -1;
  }
  if (len > 0 && buf == NULL){
    return -1;
  }
  if (len == 0 && buf == NULL){
    return 0;
  }

  //flag to check cache enabled
  bool cache_created = cache_enabled();
  //flag to see if cache lookup succeeded
  int cache_block_located = -1;

  int num_written = 0; //bytes that will have been written into mybuf
  int checklen = len; //return an unmodified len

  //mybuf will contain each write call's bytes
  uint8_t mybuf[256];

  //jbodRead will contain parts of blocks of JBOD bytes needed when we write in the middle of a block
  uint8_t jbodRead[256];
  
  //encode the write block operation
  uint32_t opWrite = encode_op(JBOD_WRITE_BLOCK,0,0);
  
  while (len > 0){
    //now we need to find disk id, block id and find out the offset from the start of a block.
    uint32_t diskID = addr/65536;
    uint32_t blockID = (addr%65536)/256;
    uint32_t offset = addr%256;

    //jbod operation will now be set to the correct block and disk
    uint32_t opDisk = encode_op(JBOD_SEEK_TO_DISK,diskID,0);
    uint32_t opBlock = encode_op(JBOD_SEEK_TO_BLOCK, 0, blockID);

    //assume there the cache does not have the desired cache block for jbodRead by default.
    cache_block_located = -1;

    if (cache_created){
      cache_block_located = cache_lookup(diskID, blockID, jbodRead);
    }

    //we either need the bytes in the whole block, bytes from the end of the block, or a few bytes within a block
    int num_bytes_to_write_to_block = min(len, min(256, 256-offset));

    //if we start in the middle of a block, we must make sure to preserve the bytes in the block in JBOD
    if (offset != 0){
      //if the cache lookup fails, we use the jbod function to read the block into jbodRead
      if(cache_block_located == -1){
        mdadm_read(addr-offset, offset, jbodRead); //read the starting bytes of JBOD up untill we are at the address we want to write from
      }
      //lookup is a success, jbod read contains the whole block instead of the starting bytes
      memcpy(mybuf, jbodRead, offset); //append the JBOD starting bytes to my buffer
    }

    //we use memcpy to write the nunber of bytes in the main buffer to my buffer
    memcpy(mybuf + offset, buf + num_written, num_bytes_to_write_to_block);
    //mybuff + offset prevents any overwritting of bytes from the beginning of a block 
    //buf + num_written tracks the position of bytes within the provided buffer
    //num_bytes_to_write_to_block tracks the size of the write, note that it must always be less than the size of a block

    //we may need to preserve any bytes at the end of a block, this includes cases where we read within the block with and without an offset
    if (offset + len < 256){
      if(cache_block_located == -1){
        //if the lookup fails, we just use mdadm_read
        mdadm_read(addr + len, 256 - (len + offset), jbodRead); //read the ending bytes of a block
        memcpy(mybuf + (len + offset), jbodRead, 256 - (len + offset)); //copy those bytes into my buffer
      } else{
        //lookup is a success, jbod read contains the whole block instead of the ending bytes
        //we have to copy specific bytes at the end of a whole block in jbodRead
        memcpy(mybuf + (len + offset), jbodRead + (len + offset), 256 - (len + offset)); //copy those bytes into my buffer
      }
    }

    //mybuf now contains the correct block information to be written to the JBOD block

    //call jbod to set to block and disk as it would have shifted during the read calls
    jbod_client_operation(opDisk,NULL);
    jbod_client_operation(opBlock,NULL);


    //now call jbod write operation to write bytes from mybuf to JBOD
    jbod_client_operation(opWrite, mybuf);

    //if the cache is enabled, update outdated data in the cache with mybuf
    if (cache_created){
      cache_update(diskID, blockID, mybuf);
    }

    num_written += num_bytes_to_write_to_block; // we will have written more bytes after each iteration
    len -= num_bytes_to_write_to_block;       // the number of bytes that need to be written decreases after each iteration
    addr += num_bytes_to_write_to_block;      // addr points to next block or the end of the block
  }
  return checklen;
}
