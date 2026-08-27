#include "ImageToFramebufferDecoder.h"

#include <Logging.h>

bool ImageToFramebufferDecoder::validateImageDimensions(int width, int height, const std::string& format,
                                                        int maxSourceWidth, int maxSourceHeight,
                                                        int64_t maxSourcePixels) {
  const int64_t pixels = static_cast<int64_t>(width) * static_cast<int64_t>(height);
  if (width <= 0 || height <= 0 || width > maxSourceWidth || height > maxSourceHeight || pixels > maxSourcePixels) {
    LOG_ERR("IMG",
            "Image too large or invalid (%dx%d %s, pixels=%lld), max supported: width<=%d height<=%d pixels<=%lld",
            width, height, format.c_str(), static_cast<long long>(pixels), maxSourceWidth, maxSourceHeight,
            static_cast<long long>(maxSourcePixels));
    return false;
  }
  return true;
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  LOG_ERR("IMG", "Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.", feature.c_str(),
          imagePath.c_str());
}
