#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multicast.h"
#include "packet.h"

unsigned int compute_checksum(char *data, int len) {
    unsigned int sum = 0;

    for (int i = 0; i < len; i++) {
        sum += (unsigned char)data[i];
    }

    return sum;
}

int main(int argc, char *argv[]) {
    mcast_t *m = multicast_init("233.3.3.3", 5001, 5002);
    int chunk_size = 512; // default
    char *files[100];     // store file names
    int file_count = 0;

    for (int i = 1; i < argc; i++) {
        printf("Argument %d: %s\n", i, argv[i]);
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                printf("Error: -c requires a value\n");
                exit(1);
            }
            chunk_size = atoi(argv[++i]); // move to next arg
        } else {
            files[file_count++] = argv[i];
        }
    }

    if (file_count == 0) {
        printf("Usage: %s [-c chunk_size] <file1> <file2> ...\n", argv[0]);
        exit(1);
    }

    printf("Chunk size: %d\n", chunk_size);

    for (int i = 0; i < file_count; i++) {
        printf("File %d: %s\n", i, files[i]);
    }
    printf("filecount is %d\n",file_count);

    // Sending the files as packets
    for (int i = 0; i < file_count; i++) {
        char filename[100];
        sprintf(filename, "./share/%s", files[i]);
        printf("filename is %s\n",filename);
        FILE *file = fopen(filename, "rb");
        printf("file opened\n");
        if (!file) {
            perror("Error: fopen");
            exit(1);
        }

        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        rewind(file);
        printf("file rewinded\n");

        int total_chunks = (file_size + chunk_size - 1) / chunk_size; // Ceiling division
        printf("Total nbr of chunks: %d\n", total_chunks);
        

        // Send file definition packet 
        file_defn_packet_t def_pkt;
        def_pkt.packet_type = 1;
        def_pkt.file_id = i;
        def_pkt.total_chunks = total_chunks;
        def_pkt.chunk_size = chunk_size;
        strcpy(def_pkt.file_name, files[i]);

        int cnt = multicast_send(m, &def_pkt, sizeof(file_defn_packet_t));
        printf("multicast sending definition packet of size %d\n", cnt);

        int seq = 0;
        int bytes_read;
        char *buffer = malloc(chunk_size);
        while ((bytes_read = fread(buffer, 1, chunk_size, file)) > 0) {
            data_packet_t *data_pkt = malloc(sizeof(data_packet_t) + bytes_read);
            data_pkt-> packet_type = 2;
            data_pkt->file_id = i;
            data_pkt->seq_num = seq++;
            data_pkt->checksum = compute_checksum(buffer, bytes_read);
            
            data_pkt->data_size = bytes_read;
            memcpy(data_pkt->data, buffer, bytes_read);

            int snd_cnt = multicast_send(m, data_pkt, sizeof(data_packet_t) + bytes_read);
            printf("Multicast sending data packet of size %d\n", bytes_read);
            free(data_pkt); 
        }
        fclose(file);
        free(buffer);
    }
    // Shutdown the receiver/cleanup code
    // multicast_destroy(m);
    // return 0;

}