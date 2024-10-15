#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <err.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include "net.h"
#include "jbod.h"

/* the client socket descriptor for the connection to the server */
int cli_sd = -1;

/* attempts to read n bytes from fd; returns true on success and false on
 * failure */
static bool nread(int fd, int len, uint8_t *buf) {
  // read data as it comes from recv_packet
  // may not read all bytes at once, so we keep reading in a loop.
  int n = 0;
  while(n < len) {
    int r = read(fd, &buf[n], len-n);
    if (r == -1){
      printf("Error on socket read [%s]\n", strerror(errno));
      return false;
    }
    if(r == 0){
      printf("Server has closed connection to socket [%s]\n", strerror(errno));
    }
    n += r;
  }
  return true;
}

/* attempts to write n bytes to fd; returns true on success and false on
 * failure */
static bool nwrite(int fd, int len, uint8_t *buf) {
  //write the buffer into the socket descriptor
  int n = 0;
  while(n < len) {
    int r = write(fd, &buf[n], len-n);
    if (r == -1){
      printf("Error on socket write [%s]\n", strerror(errno));
      return false;
    }
    if(r == 0){
      printf("Server has closed connection to socket [%s]\n", strerror(errno));
    }
    n += r;
  }
  return true;
}

/* attempts to receive a packet from fd; returns true on success and false on
 * failure */
//not fd for file descriptor, should be sd for socket descriptor
static bool recv_packet(int fd, uint32_t *op, uint16_t *ret, uint8_t *block) {
  // we read data from the socket on fd to a buffer.
  // Remember that we can not read all data all at once
  // the first 8 bytes of the message are the header like read(fd, 8, buf)
  // but read may return -1, or maybe it reads one, two, or x bytes at a given time.
  // this is a wrapper function for nread
  // nread will handle the byte unpredictability

  u_int8_t header[HEADER_LEN];

  if(nread(fd, HEADER_LEN, header) == false){
    return false;
  }

  //we have the metadata for the packet!
  //header bit details:
  //2..5 is opcode
  //6,7 is return code
  //8...263 is the block

  //convert any multibyte structures in the packet from network to host order

  //size of packet is represented by 16 bits
  //bytes 0,1 of header is the size of packet in bytes
  u_int16_t packet_byte_size;

  //size of opcode is represented by 32 bits
  //2..5 is opcode
  u_int32_t packet_opcode;

  //return code is represented by 16 bits
  //6,7 is opcode
  u_int16_t packet_return_code;

  memcpy(&packet_byte_size, header, sizeof(u_int16_t));
  memcpy(&packet_opcode, header + 2, sizeof(u_int32_t));
  memcpy(&packet_return_code, header + 6, sizeof(u_int16_t));

  //convert each packet header into their local format
  u_int32_t opcode = ntohl(packet_opcode);
  u_int16_t return_code = ntohs(packet_return_code);

  //the op pointer is given to be filled out
  *op = opcode;

  //the return pointer is given to be filled out
  *ret = return_code;

  u_int8_t packet[JBOD_BLOCK_SIZE];
  //blocks are sent byte by byte, no need to change them
  if(opcode >> 26 == JBOD_READ_BLOCK || opcode >> 26 == JBOD_SIGN_BLOCK){
    if(nread(fd, JBOD_BLOCK_SIZE, packet) == false){
      return false;
    }
    memcpy(block, packet, JBOD_BLOCK_SIZE);
  }

  return true;

}

/* attempts to send a packet to sd; returns true on success and false on
 * failure */
static bool send_packet(int sd, uint32_t op, uint8_t *block) {

  //find the number of bytes the packet will hold
  u_int16_t byte_size = HEADER_LEN;

  //the op may be a write function. 
  // If so, we need to include the block size of the jbod in the packet byte size
  int command = op >> 26;
  if(command == JBOD_WRITE_BLOCK){
    byte_size += JBOD_BLOCK_SIZE;
  }

  //initalize a buffer with just enough bytes for the header
  u_int8_t packet[byte_size];

  //convert each packet input to their network format
  uint16_t packet_byte_size = htons(byte_size);
  uint32_t packet_opcode = htonl(op);
  uint16_t packet_return_code = htons(0); //initalized to 0, JBOD will set this field

  //copy the packet header inputs into the packet header
  memcpy(packet, &packet_byte_size, sizeof(packet_byte_size));
  memcpy(packet + 2, &packet_opcode, sizeof(packet_opcode));
  memcpy(packet + 6, &packet_return_code, sizeof(packet_return_code));

  //the packet now has a header

  //if we are writing to JBOD, then we should copy the block into the packet.
  if(command == JBOD_WRITE_BLOCK){
    memcpy(packet + 8, block, JBOD_BLOCK_SIZE);
  }

  //the packet is ready to be written
  if (nwrite(sd, byte_size, packet) == false){
    return false;
  }

  return true;
}

/* attempts to connect to server and set the global cli_sd variable to the
 * socket; returns true if successful and false if not. */
bool jbod_connect(const char *ip, uint16_t port) {

  //if a connection already exists, return false
  if(cli_sd != -1){
    return false;
  }

  //creating client socket to connect from
  int socketfd = socket(AF_INET, SOCK_STREAM, 0);

  if(socketfd == -1){
    printf("Error on socket creation [%s]\n", strerror(errno));
    return false;
  }

  //creating server address to connect to
  struct sockaddr_in sa; //server address structure
  //set server address to IP_V4
  sa.sin_family = AF_INET;
  //set server port with correct network byte structure
  sa.sin_port = htons(port);
  //ascii IP address converted to UNIX format
  if (inet_aton(ip, &(sa.sin_addr)) == 0)
  {
    printf("Error on socket creation [%s]\n", strerror(errno));
    return false;
  }

  //initialize the connection
  if (connect(socketfd, (const struct sockaddr*) &sa, sizeof(sa)) == -1){
    printf("Error on socket connect [%s]\n", strerror(errno));
    return false;
  }

  //set the global socket variable to socketfd
  cli_sd = socketfd;

  return true;
}

/* disconnects from the server and resets cli_sd */
void jbod_disconnect(void) {
  //if a connection does not exist, return immediately
  if(cli_sd == -1){
    return;
  }

  if ( close(cli_sd) == -1){
    printf("Error on socket close [%s]\n", strerror(errno));
  }
  cli_sd = -1;
}

/* sends the JBOD operation to the server and receives and processes the
 * response. */
int jbod_client_operation(uint32_t op, uint8_t *block) {

  if(send_packet(cli_sd, op, block) == false){
    printf("Send packet failed \n");
    return -1;
  }
  //JBOD initializes the value
  u_int16_t return_value;

  if(recv_packet(cli_sd, &op , &return_value, block) == false){
    printf("recv packet failed \n");
    return -1;
  }

  return return_value;
}
