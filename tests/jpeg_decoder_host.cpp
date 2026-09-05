#include "JPEGdecoder.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: jpeg_decoder_host FILE EXPECTED_WIDTH EXPECTED_ROWS\n";
    return 2;
  }

  std::ifstream input(argv[1], std::ios::binary);
  if (!input) return 2;
  std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  if (jpeg.empty()) return 2;

  TFT_eSPI sink;
  const bool decoded = JPEGdecoder(jpeg.data(), jpeg.size(), sink);
  const int expectedWidth = std::atoi(argv[2]);
  const int expectedRows = std::atoi(argv[3]);
  if (!decoded || !sink.valid || sink.maximumWidth != expectedWidth ||
      sink.pushedRows != expectedRows) {
    std::cerr << "decoded=" << decoded << " valid=" << sink.valid
              << " width=" << sink.maximumWidth
              << " rows=" << sink.pushedRows << '\n';
    return 1;
  }
  std::cout << sink.maximumWidth << 'x' << sink.pushedRows << ' '
            << std::hex << sink.pixelHash << '\n';
  return 0;
}
