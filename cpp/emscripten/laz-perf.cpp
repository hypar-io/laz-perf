// laz-perf.cpp
// javascript bindings for laz-perf
//

#include <emscripten/bind.h>
#include <iostream>
#include <cstring>

#include "header.hpp"
#include "readers.hpp"
#include "lazperf.hpp"
#include "streams.hpp"

using namespace emscripten;

class LASZip
{
	public:
		LASZip()
        {}

		void open(unsigned int b, size_t len)
        {
			char *buf = (char*) b;
			mem_file_.reset(new lazperf::reader::mem_file(buf, len));
		}

		void getPoint(int buf)
        {
			char *pbuf = reinterpret_cast<char*>(buf);
			mem_file_->readPoint(pbuf);
		}

		unsigned int getCount() const
        {
			return static_cast<unsigned int>(mem_file_->pointCount());
		}

        unsigned int getPointLength() const
        {
			return static_cast<unsigned int>(mem_file_->header().point_record_length);
        }

        unsigned int getPointFormat() const
        {
			return static_cast<unsigned int>(mem_file_->header().point_format_id);
        }

	private:
		std::shared_ptr<lazperf::reader::mem_file> mem_file_;
};

class ChunkDecoder
{
public:
    ChunkDecoder()
    {}

    void open(int pdrf, int point_length, unsigned int inputBuf)
    {
        int ebCount = point_length - lazperf::baseCount(pdrf);
        char *buf = reinterpret_cast<char *>(inputBuf);
        decomp_.reset(new lazperf::reader::chunk_decompressor(pdrf, ebCount, buf));
    }

    void getPoint(unsigned int outBuf)
    {
        char *buf = reinterpret_cast<char *>(outBuf);
        decomp_->decompress(buf);
    }

private:
    std::shared_ptr<lazperf::reader::chunk_decompressor> decomp_;
};

// ---- New: ChunkTable for streaming decompression ----
// Parses the compressed chunk table from raw bytes so JavaScript can
// stream individual chunks from the file without loading it all at once.

class ChunkTable
{
public:
    ChunkTable() {}

    // Parse the chunk table.
    //   buf       – WASM pointer to raw bytes starting at the chunk table
    //               in the file (version:u32, count:u32, compressed entries...)
    //   bufLen    – byte length of the chunk table data
    //   chunkSize – from the LAZ VLR (0 = variable chunks)
    //   totalPoints – total point count from the LAS header
    //   firstChunkFileOffset – byte offset in the file where the first
    //               compressed chunk begins (= pointDataOffset + 8)
    void parse(unsigned int buf, size_t bufLen,
               unsigned int chunkSize, double totalPoints,
               double firstChunkFileOffset)
    {
        const char *data = reinterpret_cast<const char *>(buf);

        // Read chunk table header
        uint32_t version, chunk_count;
        std::memcpy(&version, data, 4);
        std::memcpy(&chunk_count, data + 4, 4);

        if (version != 0)
            return;

        bool variable = lazperf::laz_vlr::variableChunks(chunkSize);
        uint64_t remaining = static_cast<uint64_t>(totalPoints);
        uint32_t defaultChunkPts = chunkSize;

        // Build a callback that reads from the buffer after the 8-byte header
        const unsigned char *pos = reinterpret_cast<const unsigned char *>(data + 8);
        const unsigned char *end = reinterpret_cast<const unsigned char *>(data + bufLen);

        lazperf::InputCb cb = [&pos, end](unsigned char *dst, size_t count) {
            size_t avail = static_cast<size_t>(end - pos);
            size_t n = count < avail ? count : avail;
            std::memcpy(dst, pos, n);
            pos += n;
        };

        // Use laz-perf's built-in chunk table decompressor
        std::vector<lazperf::chunk> raw = lazperf::decompress_chunk_table(
            std::move(cb), chunk_count, variable);

        // Convert delta-encoded offsets to absolute file offsets and fill in
        // point counts for fixed-chunk-size files.
        chunks_.resize(raw.size());
        uint64_t absOffset = static_cast<uint64_t>(firstChunkFileOffset);

        for (size_t i = 0; i < raw.size(); i++)
        {
            chunks_[i].offset = absOffset;
            // raw[i].offset is the compressed byte SIZE of chunk i
            uint64_t compressedSize = raw[i].offset;

            if (variable)
            {
                chunks_[i].count = raw[i].count;
            }
            else
            {
                if (remaining < defaultChunkPts)
                    chunks_[i].count = remaining;
                else
                    chunks_[i].count = defaultChunkPts;
                remaining -= chunks_[i].count;
            }

            // Next chunk starts after this one's compressed bytes
            absOffset += compressedSize;
        }
    }

    unsigned int count() const { return static_cast<unsigned int>(chunks_.size()); }

    // Return as double to avoid 32-bit overflow for byte offsets in large files
    double chunkOffset(unsigned int idx) const
    {
        return static_cast<double>(chunks_[idx].offset);
    }

    // Compressed byte size of chunk idx
    double chunkByteSize(unsigned int idx) const
    {
        if (idx + 1 < chunks_.size())
            return static_cast<double>(chunks_[idx + 1].offset - chunks_[idx].offset);
        return 0.0; // last chunk: caller should read to end of compressed data
    }

    unsigned int chunkPointCount(unsigned int idx) const
    {
        return static_cast<unsigned int>(chunks_[idx].count);
    }

private:
    struct ChunkInfo { uint64_t offset; uint64_t count; };
    std::vector<ChunkInfo> chunks_;
};


EMSCRIPTEN_BINDINGS(my_module) {
	class_<LASZip>("LASZip")
		.constructor()
		.function("open", &LASZip::open)
        .function("getPointLength", &LASZip::getPointLength)
        .function("getPointFormat", &LASZip::getPointFormat)
		.function("getPoint", &LASZip::getPoint)
		.function("getCount", &LASZip::getCount);

    class_<ChunkDecoder>("ChunkDecoder")
        .constructor()
        .function("open", &ChunkDecoder::open)
        .function("getPoint", &ChunkDecoder::getPoint);

    class_<ChunkTable>("ChunkTable")
        .constructor()
        .function("parse", &ChunkTable::parse)
        .function("count", &ChunkTable::count)
        .function("chunkOffset", &ChunkTable::chunkOffset)
        .function("chunkByteSize", &ChunkTable::chunkByteSize)
        .function("chunkPointCount", &ChunkTable::chunkPointCount);
}
