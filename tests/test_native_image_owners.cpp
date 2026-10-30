#include "common/resource_owners.hpp"
#include "tinytest.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace {
sd_image_t small_image(unsigned char value) {
    auto* pixels = static_cast<uint8_t*>(std::malloc(3));
    if (!pixels)
        throw std::bad_alloc();
    pixels[0] = value;
    pixels[1] = static_cast<unsigned char>(value + 1);
    pixels[2] = static_cast<unsigned char>(value + 2);
    return {1, 1, 3, pixels};
}

char* copied_metadata() {
    auto* ptr = static_cast<char*>(std::malloc(3));
    if (!ptr)
        throw std::bad_alloc();
    std::memcpy(ptr, "{}", 3);
    return ptr;
}
} // namespace

spec("Native image owner adoption from real sd.cpp C ABI structs")
{
    it("copies valid pixel buffers and rejects dimensions that overflow")
    {
        SDImageOwner source(small_image(42));
        SDImageOwner copy(source);
        check_true(copy.get().data != nullptr);
        check_true(copy.get().data != source.get().data);
        check_equal(copy.get().data[0], static_cast<uint8_t>(42));
        source.get().data[0] = 17;
        check_equal(copy.get().data[0], static_cast<uint8_t>(42));

        // No data may be read or allocated from a uint32 size product
        // that overflows size_t; the invalid source still owns its byte.
        auto* small = static_cast<uint8_t*>(std::malloc(1));
        if (!small)
            throw std::bad_alloc();
        small[0] = 7;
        SDImageOwner overflowing({std::numeric_limits<uint32_t>::max(),
                                  std::numeric_limits<uint32_t>::max(),
                                  std::numeric_limits<uint32_t>::max(), small});
        SDImageOwner rejected(overflowing);
        check_true(rejected.get().data == nullptr);
        check_equal(rejected.get().width, static_cast<uint32_t>(0));
    }

    it("only releases an SDImageOwner after successful vector admission")
    {
        SDImageOwner image(small_image(12));
        uint8_t* pixels = image.get().data;
        SDImageVec frames;
        frames.push_back(std::move(image));
        check_true(image.get().data == nullptr);
        check_equal(frames.count(), 1);
        check_true(frames[0].data == pixels);
        check_equal(frames[0].data[0], static_cast<uint8_t>(12));
        frames.clear();
        frames.clear();
        check_true(frames.empty());
    }

    it("moves C malloc-owned image arrays, freeing their envelope and buffers once")
    {
        auto* images = static_cast<sd_image_t*>(std::calloc(2, sizeof(sd_image_t)));
        if (!images)
            throw std::bad_alloc();
        images[0] = small_image(21);
        images[1] = small_image(33);
        uint8_t* first = images[0].data;
        uint8_t* second = images[1].data;
        SDImageVec frames;
        frames.adopt(images, 2);
        check_equal(frames.count(), 2);
        check_true(frames[0].data == first && frames[1].data == second);
        check_equal(frames[0].data[0], static_cast<uint8_t>(21));
        check_equal(frames[1].data[0], static_cast<uint8_t>(33));
        frames.clear();
        check_true(frames.empty());
    }

    it("adopts upstream result ownership without leaking metadata or pixels")
    {
        auto* result = static_cast<sd_image_generation_result_t*>(
            std::calloc(1, sizeof(sd_image_generation_result_t)));
        if (!result)
            throw std::bad_alloc();
        std::unique_ptr<sd_image_generation_result_t, decltype(&free_sd_image_generation_result)>
            owner(result, &free_sd_image_generation_result);
        result->items = static_cast<sd_image_result_t*>(
            std::calloc(2, sizeof(sd_image_result_t)));
        if (!result->items)
            throw std::bad_alloc();
        result->count = 2;
        result->items[0].image = small_image(10);
        result->items[0].generation_info = copied_metadata();
        result->items[1].image = small_image(20);
        result->items[1].generation_info = copied_metadata();

        SDImageVec frames;
        frames.adopt(owner.release());
        check_equal(frames.count(), 2);
        check_equal(frames[0].data[0], static_cast<uint8_t>(10));
        check_equal(frames[1].data[0], static_cast<uint8_t>(20));
        check_true(!frames.generation_info(0).empty());
        check_true(!frames.generation_info(1).empty());
    }

    it("adopts native video frames without altering their pixel ownership")
    {
        auto* result = static_cast<sd_video_generation_result_t*>(
            std::calloc(1, sizeof(sd_video_generation_result_t)));
        if (!result)
            throw std::bad_alloc();
        std::unique_ptr<sd_video_generation_result_t, decltype(&free_sd_video_generation_result)>
            owner(result, &free_sd_video_generation_result);
        result->frames = static_cast<sd_image_t*>(
            std::calloc(2, sizeof(sd_image_t)));
        if (!result->frames)
            throw std::bad_alloc();
        result->frame_count = 2;
        result->generation_info = copied_metadata();
        result->frames[0] = small_image(57);
        result->frames[1] = small_image(68);

        SDImageVec frames;
        frames.adopt_video(owner.release());
        check_equal(frames.count(), 2);
        check_equal(frames[0].data[0], static_cast<uint8_t>(57));
        check_equal(frames[1].data[0], static_cast<uint8_t>(68));
        check_true(!frames.generation_info(0).empty());
    }
}
