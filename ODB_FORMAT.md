OPENTRACKER ODB File Format
===========================

`opentracker` *.odb file is a binary representation of the in-memory store. 
This binary file is sufficient to completely restore `opentracker` state.

High Level Algorithm to parse ODB
=================================

At a high level, the ODB file has the following structure
<pre><code>
----------------------------------- # ODB is a binary format. There are no new lines or spaces in the file.
4f 50 45 4e 54 52 41 43 4b 45 52    # Magic String "OPENTRACKER"
30 30 30 33                         # ODB Version Number in ASCII characters. In this case, version = "0001" = 1 
-----------------------------------
FE                                  # FE = Opcode that indicates following is a torrent information.
----------------------------------- # Torrent information starts from here.
00 02 2e 33 01 c4 a1 df bb 82 
1f 51 0f b5 b6 02 6f 93 6e 9f       # 20 byte info_hash of torrent
----------------------------------- 
e2 37 59 01 00 00 00 00             # Last access time in minutes since UNIX epoch, 8 bytes.
                                    # At present, when loading ODB file, just use current clock, ignore this.
-----------------------------------
00 00 00 00 00 00 00 00             # Seeding peer count for current torrent. 8 bytes long integer in little endian. 
                                    # NOT used at present.
-----------------------------------
00 00 00 00 00 00 00 00             # Total peer count for current torrent. 8 bytes long integer in little endian.
                                    # NOT used at present.
-----------------------------------
00 00 00 00 00 00 00 00             # Download times of files in current torrent. 8 bytes long integer in little endian.
                                    # NOT used at present.
-----------------------------------
01 00 00 00                         # Peer count in current peer, 4 bytes integer in little endian.
----------------------------------- # Peers information starts from here.
7f 00 00 01                         # 4 bytes ip address in network byte order. In this case, 0x7f000001 = "127.0.0.1"
1b 31                               # 2 bytes port in network byte order. In this case, 0x1b31 = 6961.
80                                  # Flag of peers. SEEDING = 0x80, COMPLETED = 0x40, STOPPED = 0x20, LEECHING = 0x00
00                                  # Reserved. Just set zero.
-----------------------------------
...                                 # Other peers information.
-----------------------------------
FE                                 
----------------------------------- 
...                                 # Other torrent information.
-----------------------------------
FF                                  # EOF opcode.
</code></pre>
