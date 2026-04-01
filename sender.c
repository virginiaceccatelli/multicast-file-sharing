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

    printf("Chunk size: %d bytes, sending %d file(s)\n", chunk_size, file_count);

    for (int i = 0; i < file_count; i++) {
        char filename[256];
        snprintf(filename, sizeof(filename), "./share/%s", files[i]);
        FILE *f = fopen(filename, "rb");
        if (!f) { perror(filename); exit(1); }
        fseek(f, 0, SEEK_END);
        printf("File %d: %s (%ld bytes)\n", i, files[i], ftell(f));
        fclose(f);
    }

    int cycle = 0;

    while (1) {
        printf("\nCycle %d\n", cycle++);
        for (int i = 0; i < file_count; i++) {
            char filename[100];
            sprintf(filename, "./share/%s", files[i]);
            printf("filename is %s\n",filename);
            FILE *file = fopen(filename, "rb");
            if (!file) { perror(filename); continue; } // skip this file this cycle, try again next

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
            def_pkt.file_name[sizeof(def_pkt.file_name) - 1] = '\0'; // guarantee null termination

            multicast_send(m, &def_pkt, sizeof(file_defn_packet_t));
            printf("Sent definition for '%s': %d chunks\n", files[i], total_chunks);

            usleep(1000); // 1ms pause after definition packet before data starts

            // Send all chunks
            char *buf = malloc(chunk_size);
            int seq = 0;
            int bytes_read;

            while ((bytes_read = fread(buf, 1, chunk_size, file)) > 0) {
                data_packet_t *pkt = malloc(sizeof(data_packet_t) + bytes_read);
                pkt->packet_type = 2;
                pkt->file_id = i;
                pkt->seq_num = seq++;
                pkt->data_size = bytes_read;
                pkt->checksum = compute_checksum(buf, bytes_read);
                memcpy(pkt->data, buf, bytes_read);

                multicast_send(m, pkt, sizeof(data_packet_t) + bytes_read);
                free(pkt);

                usleep(500); // 0.5ms between chunks to prevent UDP buffer overflow
            }

            printf("Sent %d chunks for '%s'\n", seq, files[i]);
            fclose(file);
            free(buf);
        }

        sleep(2); // 2 second pause between full cycles: prevents the sender from flooding the network
    }

    return 0;
}