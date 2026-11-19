# Movies

The 17 movies are played by the game's own movie player (sly1-decomp
`src/main/movie_player.cpp`), which the port builds unchanged but for the named patches below.
What the port supplies is what the PS2 had beside it: the MPEG library's calls (which drove the
IPU, the PS2's decoding hardware) and the sound driver's ring buffer. Nothing here comes from
another decoder: the video decoder is written from the public standard (ISO/IEC 13818-2), and
what the movies are was read from the disc's bytes.

## What a movie is

An MPEG-2 program stream, ended by the program end code. Two streams in it:

- **video** (stream 0xE0): MPEG-2, 640 by 480, 29.97 pictures a second, I, P and B pictures
  (`IPBBPBB...`), frame pictures with frame prediction and the frame transform only, 4:2:0,
  the non-linear quantiser scale, the alternate scan, the second coefficient table for intra
  pictures, the default matrices. The stream ends with the sequence end code;
- **sound** (private stream 0xBD): each packet's payload is four bytes and then data. The data
  starts with a header of 0x28 bytes (`SShd`: the kind 1, 48,000 Hz, 2 channels, an interleave
  of 0x200; then `SSbd` and a size) and goes on as 16-bit samples, 0x200 bytes of the left
  channel and 0x200 of the right in turn.

## The path

| Step | Whose | Where |
| --- | --- | --- |
| the movie's sectors read, 0x4000 bytes at a time | the game | `PumpStreamReader` |
| the program stream split into its two streams' packets | the port | `port/video/movie.cpp`, `DemultiplexBuffer` |
| a video packet into the video ring, a sound packet into the sound's | the game | the two handlers it registers |
| the video ring sent to the IPU by DMA | the game; the port takes the bytes there | `IpuDmaSink::Put` (a patch calls `port::movie::VideoBytes`) |
| a picture decoded | the port | `port/video/mpeg2.cpp`, through `DecodeMpegPicture` |
| the picture converted to 32-bit pixels, 16 by 16 blocks, a column after another | the port | `Convert`, as the IPU's conversion left it |
| the blocks uploaded to the GS and drawn as one sprite, two display frames each | the game | `UploadMovieFrame`, `DrawMovieFrame`, its decode thread |
| the sound copied into the driver's ring (commands 0x3B to 0x40, a transfer to the IOP) | the game; the port keeps the ring and mixes it | `port/audio/audio.cpp`, `MovieRing` |

The demultiplexer stops at a packet its handler refuses and is given it again, keeps the start
of a packet cut by the buffer's end (in the library's work memory, which is the game's), and
never takes the program end code: every call after it takes nothing, which is how the game's
feeding loop comes to its end. The sound's handler returns nothing (`PassMovieDataToStream` is
declared void), so a sound packet always counts as taken.

The decoder gives pictures in display order, each when the next one's start has been read, and
asks for more of the stream through the callback the game registered (`FlushMovieToIpu`).

The sound is mixed at its own rate, a little faster or slower (half a percent at most) as the
ring fills or empties: the pictures go by the display, 60 a second on the Switch where the
movie has 59.94 fields.

## The patches (`tools/patches.py`)

- `IpuDmaSink::Put`: the bytes it sends the IPU are given to the port's decoder.
- The three calls of `DrainRingBuffer` in the movie player go to a copy of it that calls the
  sink's own class. The player's two sinks are C++ classes with virtual functions;
  `ring_buffer.cpp` calls a sink through GCC 2.95's method table, which the classes do not have
  when built with today's compilers.
- `DrawMovieFrame`: in the 16:9 picture the movie keeps its 4:3 shape, in the middle.

## Checked

- `port/video/mpeg2.cpp` against a reference decoder on the first movie's 409 pictures: the
  greatest difference of any sample is 5 of 255, the mean 0.007 (the inverse transforms round
  differently); both coefficient tables are complete prefix codes. 0.9 ms a picture on the host.
- The opening on the host: the first movie plays to its end (409 pictures, 2,092 video packets,
  645 of sound, no error in the stream), its sound is in the file written, and the game goes on.

## Not yet

- Seen and heard on the Switch; its cost there.
- The other 16 movies (the same coding by their headers; two were read).
