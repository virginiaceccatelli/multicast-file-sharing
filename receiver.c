#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "multicast.h"
#include "packet.h"
#include <time.h>

typedef struct {
    int packets_received;
    int bytes_received;
    int corrupted;
    int files_completed;
    struct timespec start;
    struct timespec file_first_chunk[MAX_FILES];
} receiver_stats_t;

unsigned int compute_checksum(char *data, int len) {
    unsigned int sum = 0;
    for (int i = 0; i < len; i++) {
        sum += (unsigned char)data[i];
    }
    return sum;
}

static void print_receiver_stats(receiver_stats_t *stats, int file_id, int total_chunks) {
    stats->files_completed++;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double latency = (now.tv_sec  - stats->file_first_chunk[file_id].tv_sec) +
    (now.tv_nsec - stats->file_first_chunk[file_id].tv_nsec) / 1e9;
    printf("\nRECEIVER STATS (file %d)\n", file_id);
    printf("  Files completed:  %d\n", stats->files_completed);
    printf("  Packets received: %d\n", stats->packets_received);
    printf("  Bytes received:   %d\n", stats->bytes_received);
    printf("  Corrupted:        %d\n", stats->corrupted);
    printf("  File latency:     %.2f sec\n", latency);
    printf("  Throughput:       %.1f chunks/sec\n", total_chunks / latency);
}

static void init_file_state(file_state_t *s) {
    s->total_chunks = 0;
    s->received_chunks = 0;
    s->chunks = NULL;
    s->chunk_sizes = NULL;
    s->general_chunk_size = 0;
    s->done = 0;
    memset(s->file_name, 0, sizeof(s->file_name));
}

static int is_complete(file_state_t *s) {
    if (s->chunks == NULL || s->total_chunks <= 0) {
        return 0;
    }

    for (int i = 0; i < s->total_chunks; i++) {
        if (s->chunks[i] == NULL) {
            return 0;
        }
    }
    return 1;
}

static void send_retrans_request(mcast_t *m,
                                 int file_id,
                                 int seq,
                                 const char *filename,
                                 const char *label) {
    retrans_packet_t req;
    memset(&req, 0, sizeof(req));
    req.packet_type = 3;
    req.file_id = file_id;
    req.seq_num = seq;
    strncpy(req.filename, filename, sizeof(req.filename) - 1);

    multicast_send(m, &req, sizeof(req));
    printf("[%s] Requested retransmission: file_id=%d seq=%d file='%s'\n",
           label, file_id, seq, req.filename);
}

static void send_retrans_complete(mcast_t *m, int file_id, const char *label) {
    retrans_recvd_packet_t pkt;
    pkt.packet_type = 5;
    pkt.file_id = file_id;
    multicast_send(m, &pkt, sizeof(pkt));
    printf("[%s] Sent retransmission-complete packet for file_id=%d\n",
           label, file_id);
}

static int save_file(file_state_t *s, const char *out_dir, const char *label) {
    if (s->done) {
        return 1;
    }

    mkdir(out_dir, 0777);

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", out_dir, s->file_name);

    // checksum calculation for entire file
    unsigned int computed = 0;
    for (int i = 0; i < s->total_chunks; i++)
        computed += compute_checksum(s->chunks[i], s->chunk_sizes[i]);
    if (computed != s->file_checksum) {
        printf("[%s] Integrity check FAILED for '%s' (got %u expected %u)\n",
            label, s->file_name, computed, s->file_checksum);
        return 0;
    }


    FILE *out = fopen(path, "wb");
    if (!out) {
        perror("fopen");
        return 0;
    }

    for (int i = 0; i < s->total_chunks; i++) {
        fwrite(s->chunks[i], 1, s->chunk_sizes[i], out);
    }

    fclose(out);
    s->done = 1;

    // memory cleanup for file state
    for (int i = 0; i < s->total_chunks; i++) {
        free(s->chunks[i]);
        s->chunks[i] = NULL;
    }
    free(s->chunks);
    free(s->chunk_sizes);
    s->chunks = NULL;
    s->chunk_sizes = NULL;
    
    printf("[%s] File saved: %s\n", label, path);
    return 1;
}

int main(int argc, char *argv[]) {
    const char *out_dir = "./received_files";
    const char *label = "receiver";

    if (argc >= 2) {
        out_dir = argv[1];
    }
    if (argc >= 3) {
        label = argv[2];
    }

    setbuf(stdout, NULL);
    printf("[%s] Receiver is listening...\n", label);
    printf("[%s] Output directory: %s\n", label, out_dir);

    mcast_t *m = multicast_init("239.255.0.1", 5002, 5001);
    multicast_setup_recv(m);

    receiver_stats_t stats = {0};
    clock_gettime(CLOCK_MONOTONIC, &stats.start);

    char buffer[MAX_PACKET_SIZE];
    file_state_t state[MAX_FILES];

    for (int i = 0; i < MAX_FILES; i++) {
        init_file_state(&state[i]);
    }

    mkdir(out_dir, 0777);

    while (1) {
        if (multicast_check_receive(m) > 0) {
            int n = multicast_receive(m, buffer, sizeof(buffer));
            if (n <= 0) {
                continue;
            }

            int type = *(int *)buffer;
            printf("\n[%s] Received packet of size %d, type %d\n", label, n, type);

            if (type == 1) {
                file_defn_packet_t *def_pkt = (file_defn_packet_t *)buffer;
                int file_id = def_pkt->file_id;

                if (state[file_id].done) {
                    printf("[%s] File %d already completed, ignoring definition\n", label, file_id);
                    continue;
                }

                if (file_id < 0 || file_id >= MAX_FILES) {
                    printf("[%s] Invalid file_id %d\n", label, file_id);
                    continue;
                }

                if (state[file_id].chunks != NULL && !state[file_id].done) {
                    printf("[%s] Already tracking file_id=%d, ignoring duplicate definition\n",
                           label, file_id);
                    continue;
                }

                if (state[file_id].chunks != NULL) {
                    for (int j = 0; j < state[file_id].total_chunks; j++) {
                        free(state[file_id].chunks[j]);
                    }
                    free(state[file_id].chunks);
                    free(state[file_id].chunk_sizes);
                    init_file_state(&state[file_id]);
                }

                state[file_id].file_checksum = def_pkt->file_checksum;
                state[file_id].total_chunks = def_pkt->total_chunks;
                state[file_id].received_chunks = 0;
                state[file_id].general_chunk_size = def_pkt->chunk_size;
                state[file_id].done = 0;

                strncpy(state[file_id].file_name,
                        def_pkt->file_name,
                        sizeof(state[file_id].file_name) - 1);
                state[file_id].file_name[sizeof(state[file_id].file_name) - 1] = '\0';

                state[file_id].chunks = calloc(def_pkt->total_chunks, sizeof(char *));
                state[file_id].chunk_sizes = calloc(def_pkt->total_chunks, sizeof(int));

                if (!state[file_id].chunks || !state[file_id].chunk_sizes) {
                    perror("calloc");
                    return 1;
                }

                printf("[%s] Initialized state for file '%s' (%d chunks)\n",
                       label,
                       state[file_id].file_name,
                       state[file_id].total_chunks);

            } else if (type == 2) {
                data_packet_t *d_pkt = (data_packet_t *)buffer;
                int file_id = d_pkt->file_id;
                int seq = d_pkt->seq_num;

                if (file_id < 0 || file_id >= MAX_FILES) {
                    printf("[%s] Invalid file_id %d\n", label, file_id);
                    continue;
                }

                if (state[file_id].chunks == NULL) {
                    printf("[%s] Received data before definition for file_id %d\n",
                           label, file_id);
                    continue;
                }

                if (seq < 0 || seq >= state[file_id].total_chunks) {
                    printf("[%s] Invalid seq %d for file_id %d\n", label, seq, file_id);
                    continue;
                }

                stats.packets_received++;
                stats.bytes_received += n;

                if (compute_checksum(d_pkt->data, d_pkt->data_size) != d_pkt->checksum) {
                    printf("[%s] Corrupted chunk %d for file '%s'\n",
                           label, seq, state[file_id].file_name);
                    stats.corrupted++;
                    continue;
                }

                if (state[file_id].chunks[seq] == NULL) {
                    state[file_id].chunks[seq] = malloc(d_pkt->data_size);
                    if (!state[file_id].chunks[seq]) {
                        perror("malloc");
                        return 1;
                    }

                    if (state[file_id].received_chunks == 0) {
                        clock_gettime(CLOCK_MONOTONIC, &stats.file_first_chunk[file_id]);
                    }

                    memcpy(state[file_id].chunks[seq], d_pkt->data, d_pkt->data_size);
                    state[file_id].chunk_sizes[seq] = d_pkt->data_size;
                    state[file_id].received_chunks++;

                    printf("[%s] Stored chunk %d for '%s' (%d/%d)\n",
                           label,
                           seq,
                           state[file_id].file_name,
                           state[file_id].received_chunks,
                           state[file_id].total_chunks);

                    if (state[file_id].received_chunks == state[file_id].total_chunks) {
                        int total_chunks = state[file_id].total_chunks;
                        if (save_file(&state[file_id], out_dir, label)) {
                            send_retrans_complete(m, file_id, label);
                            print_receiver_stats(&stats, file_id, total_chunks);
                        }
                    }
                } else {
                    printf("[%s] Chunk %d for '%s' already stored, ignoring duplicate\n",
                           label, seq, state[file_id].file_name);
                }

            } else if (type == 4) {
                end_packet_t *e_pkt = (end_packet_t *)buffer;
                int file_id = e_pkt->file_id;

                if (file_id < 0 || file_id >= MAX_FILES) {
                    printf("[%s] Invalid file_id %d\n", label, file_id);
                    continue;
                }

                if (state[file_id].chunks == NULL) {
                    // printf("[%s] End packet received before definition for file_id %d\n",
                    //        label, file_id);
                    continue;
                }

                int total_chunks = state[file_id].total_chunks;
                int was_done = state[file_id].done; 
                if (save_file(&state[file_id], out_dir, label)) {
                    if (!was_done) {  // only do anything if this call actually completed the file
                        send_retrans_complete(m, file_id, label);
                        print_receiver_stats(&stats, file_id, total_chunks);
                    }
                } else {
                    printf("[%s] File '%s' incomplete at end packet (%d/%d)\n",
                           label,
                           state[file_id].file_name,
                           state[file_id].received_chunks,
                           state[file_id].total_chunks);

                    for (int j = 0; j < state[file_id].total_chunks; j++) {
                        if (state[file_id].chunks[j] == NULL) {
                            send_retrans_request(m, file_id, j, state[file_id].file_name, label);
                            usleep(10000);
                        }
                    }
                }

            } else if (type == 3) {
                retrans_packet_t *req = (retrans_packet_t *)buffer;
                printf("[%s] Ignoring retrans request seen on receiver: file_id=%d seq=%d\n",
                       label, req->file_id, req->seq_num);

            } else if (type == 5) {
                retrans_recvd_packet_t *pkt = (retrans_recvd_packet_t *)buffer;
                printf("[%s] Ignoring retransmission-complete packet on receiver for file_id=%d\n",
                       label, pkt->file_id);

            } else {
                printf("[%s] Unknown packet type %d\n", label, type);
            }
        }
    }

    return 0;
}