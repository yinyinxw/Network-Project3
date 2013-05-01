// EE122 Project 2 - sender.c
// Xiaodian (Yinyin) Wang and Arnab Mukherji
//
// sender.c is the sender sending the packets to the router

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/fcntl.h>
#include <math.h>
#include "common.h"

//Input Arguments to sender.c:
//argv[1] is Sender ID, which is either 1 (for Sender1) or 2 (for Sender2)
//argv[2] is the mean value inter-packet time R in millisec (based on Poisson distr). 
//argv[3] is the receiver ID, which is either 1 (Receiver1) or 2 (Receiver2)
//argv[4] is the router IP
//argv[5] is the size of the sliding window for Go-Back-N ARQ (default should be 32 packets)
//argv[6] is the initial timeout time (in milliseconds) for Go-Back-N
 // ARQ (it is only used once, after the first packet is received, the
 // timeout time is estimated using adaptive exponential averaging

int main(int argc, char *argv[]) {
    //Variables used for input arguments
    unsigned int sender_id; 
    unsigned int r; //inter-packet time W w/ mean R
    unsigned int receiver_id;
    char *dest_ip; //destination/router IP
    unsigned int slide_window_size;
    unsigned int timeout_time;
    
    //Variables used for establishing the connection
    int sockfd, listen_sockfd;
    struct addrinfo hints, *receiver_info, *sender_info;
    int return_val, sender_return_val;
    struct sockaddr_storage their_addr;
    socklen_t addr_len;
    
    //Variabes used for outgoing packets
    int packet_success;
    struct msg_payload *buffer;
    struct msg_payload payload;
    struct timeval start_time;
    struct timeval curr_time;
    time_t delta_time = 0, curr_timestamp_sec = 0, curr_timestamp_usec = 0;
    //Variables used for incoming packets
    int recv_success;
    struct msg_payload *buff;
    
    //Variables used for the sliding window Go-Back-N ARQ
    unsigned int next_seq_no = 0, beg_seq_no = 0, recv_no = 0;
    
    //Variables used for estimation of packet timeout value
    unsigned int ack_pkt_cnt = 0;
    unsigned float avg_RTT = 0, deviation = 0;
    
    //Parsing input arguments
    if (argc != 7) {
        perror("Sender 2: incorrect number of command-line arguments\n");
        return 1; 
    } else {
        sender_id = atoi(argv[1]);
        r = atoi(argv[2]);
        receiver_id = atoi(argv[3]);
        dest_ip = argv[4];
        slide_window_size = atoi(argv[5]);
        timeout_time = atoi(argv[6]);
        printf("Sender id %d, r value %d, receiver id %d, router IP address %s, port number %s, sliding window size is %d, the timeout time is %d\n", sender_id, r, receiver_id, dest_ip, ROUTER_PORT, slide_window_size, timeout_time);
    }
    
    //load struct addrinfo with host information
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    
    /*
     * for DEBUGGING purposes: change ROUTER_PORT TO get_receiver_port(1 or 2)
     * to have the sender directly send packets to the receiver.
     */
    //Get target's address information
    if ((return_val = getaddrinfo(dest_ip, ROUTER_PORT, &hints,
                          &receiver_info)) != 0) {
        perror("Sender 2: unable to get target's address info\n");
        return 2;
    }
    
    //Take fields from first record in receiver_info, and create socket from it.
    if ((sockfd = socket(receiver_info->ai_family,receiver_info->ai_socktype,receiver_info->ai_protocol)) == -1) {
        perror("Sender 2: unable to create sending socket\n");
        return 3;
    }
    
    //Creating the listening socket
    if ((sender_return_val = getaddrinfo(NULL, SENDER_PORT, &hints, &sender_info)) != 0) {
        perror("Sender 2: unable to get address info for listening socket\n");
        return 4;
    }
    if((listen_sockfd = socket(sender_info->ai_family, sender_info->ai_socktype, sender_info->ai_protocol)) == -1) {
        perror("Sender 2: unable to create listening socket\n");
        return 5;
    }
    //set listening socket to be nonblocking
    fcntl(listen_sockfd, F_SETFL, O_NONBLOCK);
    
    if ((bind(listen_sockfd, sender_info->ai_addr, sender_info->ai_addrlen)) == -1) {
        close(listen_sockfd);
        perror("Sender 2: unable to bind listening socket to port\n");
        return 6;
    }
    printf("Sender 2: waiting to receive ACKS from D2...\n");
    
    //Establishing the packet: filling basic packet information
    memset(&payload, 0, sizeof payload);
    buffer = &payload;
    buffer->sender_id = htonl(sender_id); //Sender ID
    buffer->receiver_id = htonl(receiver_id); //Receiver ID

    addr_len = sizeof their_addr;
    //Memory set timeval structs for calculuating elapsed time
    memset(&curr_time, 0, sizeof (struct timeval));
    memset(&start_time, 0, sizeof (struct timeval));
    gettimeofday(&start_time, NULL);
    //allocate memory to buffer incoming ACK packets
    buff = malloc(sizeof (struct msg_payload));
    memset(buff, 0, sizeof (struct msg_payload));
    
    while (1) {
        //delta_time is elapsed time in milliseconds
        delta_time = (curr_time.tv_sec * 1000 + curr_time.tv_usec) - (start_time.tv_sec * 1000 + start_time.tv_usec);
        
        if (next_seq_no < (beg_seq_no + slide_window_size)) {
            buffer->seq = htonl(next_seq_no); //pkt sequence ID, initialized at 0
            //Get the current packet timestamp
            gettimeofday(&curr_time, NULL);
            curr_timestamp_sec = curr_time.tv_sec;
            curr_timestamp_usec = curr_time.tv_usec;
            buffer->timestamp_sec = htonl(curr_timestamp_sec); //Pkt timestamp_sec
            buffer->timestamp_usec = htonl(curr_timestamp_usec); //Pkt timestamp_usec
            printf("Pkt data: seq#-%d, senderID-%d, receiverID-%d, timestamp_sec-%d, timestamp_usec %d\n", ntohl(buffer->seq), ntohl(buffer->sender_id), ntohl(buffer->receiver_id), ntohl(buffer->timestamp_sec), ntohl(buffer->timestamp_usec));
            //Send packet
            packet_success = sendto(sockfd, buffer, sizeof(struct msg_payload), 0, receiver_info->ai_addr, receiver_info->ai_addrlen);
            printf("Sender 2: Total packets sent so far: %d\n",next_seq_no);
            poisson_delay((double)r);
            //Update the packet sequence ID
            next_seq_no++;
        }
        
        recv_success = recvfrom(listen_sockfd, buff, sizeof (struct msg_payload), 0, (struct sockaddr *)&their_addr, &addr_len);
        if (recv_success > 0) {//received ACK packet
            buff->seq = ntohl(buff->seq);
            /*Shift the window if Sender 2 receives an ACK for a 
             packet sequence ID outside of the current window, or if 
             the packet sequence ID is smaller than the current 
             sequence number (next_seq_no) and S2 has timed out*/
            if ((buff->seq < next_seq_no && delta_time >= timeout_time)|| (buff->seq > (beg_seq_no + slide_window_size))) {
                beg_seq_no = buff->seq;
                next_seq_no = buff->seq;
                //reset the timer
                gettimeofday(&curr_time, NULL);
                gettimeofday(&start_time, NULL);
            }
        }
    }
    close(sockfd);
    close(listen_sockfd);
    return 0; 
}