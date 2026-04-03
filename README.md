## Multicast File Transfer System

gcc sender.c multicast.c -o sender
gcc receiver.c multicast.c -o receiver

### Packet Types

type 1 = file definition packet
type 2 = data chunk packet
type 3 = retransmission request
type 4 = end-of-file packet
type 5 = retransmission-complete acknowledgement

### Sender 
./sender -c [chunk size, optional] [filename(s) to send]

### Receiver
./receiver [out_dir, optional (default: received_files)]

