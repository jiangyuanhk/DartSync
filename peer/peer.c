//FILE: peer/peer.c
//
//Description: This file implements a peer in the DartSync project.
//
//Date: May 22, 2015

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netdb.h>
#include <assert.h>
#include <sys/utsname.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>

#include "peer.h"
#include "../common/constants.h"
#include "../common/pkt.h"
#include "../common/filetable.h"
#include "../common/peertable.h"
#include "peer_helpers.h"


// Globals Variables
int tracker_connection;     // socket connection between peer and tracker so can send / receive between the two
int heartbeat_interval;
int piece_len;

fileTable_t* filetable;     //local file table to keep track of files in the directory
peerTable_t* peertable;     //peer table to keep track of ongoing downloading tasks


//Function to connect the peer to the tracker on the HANDSHAKE Port.
// Returns -1 if it failed to connect.  Otherwise, returns the sockfd
int connect_to_tracker() {
  int tracker_connection;
  struct sockaddr_in servaddr;
  struct hostent *hostInfo;

  //Get the host name by requesting user input and check if valid
  char hostname[MAX_HOSTNAME_SIZE];
  printf("Enter hostname of the tracker to connect to:");
  scanf("%s",hostname);

  hostInfo = gethostbyname(hostname);
  if (!hostInfo) {
    printf("Error with the hostname of the tracker!\n");
    return -1;
  }
    
  // Set up the tracker address info so we can connect to it
  servaddr.sin_family = hostInfo->h_addrtype;  
  memcpy((char *) &servaddr.sin_addr.s_addr, hostInfo->h_addr_list[0], hostInfo->h_length);
  servaddr.sin_port = htons(HANDSHAKE_PORT);

  // Create socket on local host to connect to the tracker
  tracker_connection = socket(AF_INET, SOCK_STREAM, 0);  
  if (tracker_connection < 0) {
    return -1;
  }

  printf("Attempting to connect to the tracker.\n");
  if (connect (tracker_connection, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0){
    return -1;
  }

  printf("Successfully connected to the tracker.\n");
  return tracker_connection; 
}

//Thread to listen for messages from the tracker.  Upon receiving messages from the tracker, it looks
// to sync the local files with the tracker file knowledge, creating download threads as necessary.
void* tracker_listening(void* arg) {
  printf("Start Tracker Listening Thread. \n");
  ptp_tracker_t* pkt = malloc(sizeof(ptp_tracker_t));

  fileTable_t* tracker_filetable = NULL;
  //continuously receive packets from the tracker
  while(pkt_peer_recvPkt(tracker_connection, pkt) > 0) {
    printf("Received a packet from the tracker\n");
    
    //extract the file table from the packet from the tracker
    tracker_filetable = filetable_convertEntriesToFileTable(pkt -> filetableHeadPtr);
    printf("Received filetable:\n");
    filetable_printFileTable(tracker_filetable);
    //loop through the master file table to see which files need to be synchronized locally
    fileEntry_t* file = tracker_filetable -> head;
    while (file != NULL) {

      //check to see if the file exists locally
      fileEntry_t* local_file = filetable_searchFileByName(filetable, file -> file_name); 
      
      // download the updated file if the local file does not exist or the local file is outdated
      if(local_file == NULL || (file -> timestamp) > (local_file -> timestamp) ) { 
        printf("File updated or added.  Need to download file: %s\n", file -> file_name);
        // //TODO make sure that the file is not already being downloaded and if not, add to the peer table
        // pthread_t p2p_download_thread;
        // pthread_create(&p2p_download_thread, NULL, p2p_download, file);
      }

      file = file -> next;  //move to next item in file table from tracker
    }

    printf("Finished determining files that need to be uploaded.\n");
    //Delete files that are in local table, but not in the file table from the tracker
    file = filetable -> head;
    while (file != NULL) {

      //check to see if the local file exists in the tracker table
      fileEntry_t* master_file = filetable_searchFileByName(tracker_filetable, file -> file_name);

      //if the file is not in the master file table and is locally, then delete it
      if (master_file == NULL) {

         if( remove(file -> file_name) == 0) {
          printf("Successfully removed the file in filesystem: %s \n", file -> file_name);
          file = file -> next;
          filetable_deleteFileEntryByName(filetable, file -> file_name);
         }

         else{
          file = file -> next;
          printf("Error in removing the file from the file system.\n");
         }
      }
      file = file -> next;
    }
    printf("Finished deleting files.\n");
  }

  free(pkt);
  if (tracker_filetable) filetable_destroy(tracker_filetable);
  pthread_exit(NULL);
}


// Thread to listen on the P2P port for download requests from other peers to spawn upload threads.
// First initializes a port to listen for requests to connect to it and then listens.
void* p2p_listening(void* arg) {
  
  //Set up the socket for listening
  int peer_sockfd;
  struct sockaddr_in local_peer_addr;

  struct sockaddr_in other_peer_addr;
  socklen_t other_peer_addr_len;

  peer_sockfd = socket(AF_INET, SOCK_STREAM, 0); 
  if (peer_sockfd < 0) {
    pthread_exit(NULL);
  }

  //Set up the local addr info
  memset(&local_peer_addr, 0, sizeof(local_peer_addr));
  local_peer_addr.sin_family = AF_INET;
  local_peer_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  local_peer_addr.sin_port = htons(P2P_PORT);

  //Attempt to bind the socket
  if (bind (peer_sockfd, (struct sockaddr *)&local_peer_addr, sizeof(local_peer_addr)) < 0){
    printf("Failed to bind.\n");
    pthread_exit(NULL);
  }

  //Attempt to set up socket for listening
  if (listen (peer_sockfd, MAX_NUM_PEERS) < 0) {
    printf("Failed to listen.\n");
    pthread_exit(NULL);
  }
  
  //Start infinite loop waiting to accept connections from peers.
  //When a peer connects, we create a upload thread to handle that connection.  
  while(1) {
    int peer_conn;

    peer_conn = accept(peer_sockfd, (struct sockaddr*) &other_peer_addr, &other_peer_addr_len);
    printf("Established a connection to a peer. Starting a P2PDownload Thread.\n");

    //start the p2p_download_thread
    pthread_t p2p_upload_thread;
    pthread_create(&p2p_upload_thread, NULL, p2p_upload, &peer_conn);
  }

  pthread_exit(NULL);
}


/* Thread to download a file from a peer.  First establishes the connecting with a peer.
   Then, sends a file_metadata_t to the peer to let it know the name of the file it needs to download.
   Next, it receives a file_metadata_t from the peer to let it know its about to receive the data and containing 
   information about the start and send_size as well as the file name.  Then, it receives the file and closes
   the connection */
void* p2p_download(void* arg) {
  fileEntry_t* file = (fileEntry_t*) arg;

  struct sockaddr_in servaddr;
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = inet_addr(file -> iplist[0]);      //just use the first ip in the list
  servaddr.sin_port = htons(P2P_PORT);

  int peer_conn = socket(AF_INET, SOCK_STREAM, 0);  

  if(peer_conn < 0) {
    printf("Error creating socket in p2p download.\n");
    pthread_exit(NULL);
  }

  if( connect(peer_conn, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0){
    printf("Failed to connect ot local ON process.\n");
    pthread_exit(NULL);
  }

  printf("Connected to a peer upload thread.\n");

  //Download data from the peer
  //Send the name of the file you need to download
  file_metadata_t* meta_info  = send_meta_data_info(peer_conn, file -> file_name, 0, 0);
  free(meta_info);

  //Recv the file
  file_metadata_t* metadata = calloc(1, sizeof(file_metadata_t));
  int ret1 = receive_meta_data_info(peer_conn, metadata);
  int ret2 = receive_data_p2p(peer_conn, metadata);
  printf("Ret1: %d    Ret2: %d \n", ret1, ret2);
  free(metadata);

  //TODO
  //handshake with the tracker
  //send the filetable so that I can be added to the iplist[0]

  //TODO
  //remove this from the peer table so know that the download is completed
  //close the connection

  close(peer_conn);
  pthread_exit(NULL);
}


/* Thread to upload a file to a peer. First accepts the connection from a peer.
   Then, receives a file_metadata_t from the peer to let it know the name of the file it needs to upload.
   Next, it sends a file_metadata_t to the peer to let it know its about to send the data and containing 
   information about the start and send_size as well as the file name.  Then, it sends the file and closes
   the connection */
void* p2p_upload(void* arg) {
  int peer_conn = *(int*) arg;  //gonna be casting issue here
  
  //get the filename of the file to send  
  file_metadata_t* recv_metadata = malloc(sizeof(file_metadata_t));
  receive_meta_data_info(peer_conn, recv_metadata);

  //Sending a file p2p 
  printf("Sending a File: %s \n", recv_metadata -> filename);
  file_metadata_t* metadata = send_meta_data_info(peer_conn, recv_metadata -> filename, 0, get_file_size(recv_metadata -> filename));
  send_data_p2p(tracker_connection, metadata);
  free(metadata);
  free(recv_metadata);
  close(peer_conn);
  pthread_exit(NULL);
}



/* Thread to monitor local file changes.  When a file is changed locally, inform the tracker by sending the filetable
  to the tracker.
*/
void* file_monitor(void* arg) {
  //initially send filetable to the tracker.
  printf("Starting the File Monitor Thread.\n");
  printf("Sending the Initial Filetable\n");

  pthread_mutex_lock(filetable -> filetable_mutex);
  ptp_peer_t* pkt = pkt_create_peerPkt();
  char ip_address[IP_LEN];
  get_my_ip(ip_address);
  pkt_config_peerPkt(pkt, FILE_UPDATE, ip_address, HANDSHAKE_PORT, filetable -> size, filetable -> head);
  pthread_mutex_unlock(filetable -> filetable_mutex);
  pkt_peer_sendPkt(tracker_connection, pkt, filetable -> filetable_mutex);
  printf("Successfully send the filetable packet.\n");


    // fileTable_t* peer_filetable = filetable_convertEntriesToFileTable(pkt_recv.filetableHeadPtr);
    // filetable_printFileTable(peer_filetable);
  //keep looping and use the file monitor thing daneil did

  //TODO Intergrate the file monitor logic from Daniel's File Monitor
  pthread_exit(NULL);
}


void* keep_alive(void* arg) {
  int interval = *(int*) arg;
  while(1) {
    sleep(interval);
    //TODO
    //send the keep alive message
  }

  pthread_exit(NULL);
}


int main(){
  
  //Initialize the local filetable to include current directory and subdirectories
  filetable = create_local_filetable(".");
  filetable_printFileTable(filetable);

  //Initialize the peer table as empty
  peerTable_t* peertable = malloc(sizeof(peerTable_t)); 

  //Attempt to establish connection with tracker
  if ( (tracker_connection = connect_to_tracker()) < 0) {
    printf("Failed to connect to tracker. Exiting\n");
    exit(0);
  }

  printf("Connected\n");

  // pthread_mutex_lock(filetable -> filetable_mutex);
  // ptp_peer_t* pkt = pkt_create_peerPkt();
  // char ip_address[IP_LEN];
  // get_my_ip(ip_address);
  // pkt_config_peerPkt(pkt, FILE_UPDATE, ip_address, HANDSHAKE_PORT, filetable -> size, filetable -> head);
  // pthread_mutex_unlock(filetable -> filetable_mutex);
  // pkt_peer_sendPkt(tracker_connection, pkt, filetable -> filetable_mutex);
  // printf("Successfully send the filetable packet.\n");

  //  Send a register packet to the tracker
  if (send_register_packet(tracker_connection) < 0) {
    printf("Failed to send register packet\n");
    return -1; // maybe exit();
  }

  printf("Successfully sent register packet.\n");

  //Receive the acknowledgement of the register packet from the tracker
  printf("Receiving registed handshake packet from the tracker.\n");
  ptp_tracker_t* packet = calloc(1, sizeof(ptp_tracker_t));
  pkt_peer_recvPkt(tracker_connection, packet);
  heartbeat_interval = packet -> heartbeatinterval;
  piece_len = packet -> piece_len;  
  printf("Inteval: %d  Piece Len: %d   FT-Size: %d \n", packet -> heartbeatinterval, packet -> piece_len, packet -> filetablesize);
  free(packet);

  // //Start the keep alive thread
  // pthread_t keep_alive_thread;
  // pthread_create(&keep_alive_thread, NULL, keep_alive, &keep_alive_interval);

  // start the thread to listen for data from the tracker
  pthread_t tracker_listening_thread;
  pthread_create(&tracker_listening_thread, NULL, tracker_listening, (void*)0);

  pthread_t file_monitor_thread;
  pthread_create(&file_monitor_thread, NULL, file_monitor, (void*)0);
  
  // start the thread to listen on the p2p port for connections from other peers
  // pthread_t p2p_listening_thread;
  // pthread_create(&p2p_listening_thread, NULL, p2p_listening, &tracker_connection);
  while(1) sleep(60);
}
