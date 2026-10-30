#include "common/resource_owners.hpp"
#include "tinytest.hpp"

#include <json.hpp>

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
    it("accepts null image and video metadata pointers without constructing invalid strings")
    {
        auto* raw_image = static_cast<sd_image_generation_result_t*>(
            std::calloc(1, sizeof(sd_image_generation_result_t)));
        if (!raw_image) throw std::bad_alloc();
        std::unique_ptr<sd_image_generation_result_t, decltype(&free_sd_image_generation_result)>
            image_owner(raw_image, &free_sd_image_generation_result);
        raw_image->items = static_cast<sd_image_result_t*>(
            std::calloc(1, sizeof(sd_image_result_t)));
        if (!raw_image->items) throw std::bad_alloc();
        raw_image->count = 1;
        raw_image->items[0].image = small_image(3);
        // generation_info intentionally remains null on this C ABI result.
        SDImageVec images;
        images.adopt(image_owner.release());
        check_equal(images.count(), 1);
        check_true(images.generation_info(0).empty());
        check_equal(images[0].data[0], static_cast<uint8_t>(3));

        auto* raw_video = static_cast<sd_video_generation_result_t*>(
            std::calloc(1, sizeof(sd_video_generation_result_t)));
        if (!raw_video) throw std::bad_alloc();
        std::unique_ptr<sd_video_generation_result_t, decltype(&free_sd_video_generation_result)>
            video_owner(raw_video, &free_sd_video_generation_result);
        raw_video->frames = static_cast<sd_image_t*>(
            std::calloc(1, sizeof(sd_image_t)));
        if (!raw_video->frames) throw std::bad_alloc();
        raw_video->frame_count = 1;
        raw_video->frames[0] = small_image(9);
        SDImageVec frames;
        frames.adopt_video(video_owner.release());
        check_equal(frames.count(), 1);
        check_true(frames.generation_info(0).empty());
        check_equal(frames[0].data[0], static_cast<uint8_t>(9));
    }

    it("retires already-moved and untouched native pixels when metadata decoding throws")
    {
        auto* raw = static_cast<sd_image_generation_result_t*>(
            std::calloc(1, sizeof(sd_image_generation_result_t)));
        if (!raw) throw std::bad_alloc();
        std::unique_ptr<sd_image_generation_result_t, decltype(&free_sd_image_generation_result)>
            source(raw, &free_sd_image_generation_result);
        raw->items = static_cast<sd_image_result_t*>(
            std::calloc(3, sizeof(sd_image_result_t)));
        if (!raw->items) throw std::bad_alloc();
        raw->count = 3;
        for (int i = 0; i < 3; ++i) {
            raw->items[i].image = small_image(static_cast<unsigned char>(15 + i));
            raw->items[i].generation_info = copied_metadata();
        }
        // Only the second metadata conversion fails; the third image has not
        // yet been transferred when the C result guard is destroyed.
        std::free(raw->items[1].generation_info);
        raw->items[1].generation_info = static_cast<char*>(std::malloc(2));
        if (!raw->items[1].generation_info) throw std::bad_alloc();
        std::memcpy(raw->items[1].generation_info, "!", 2);

        SDImageVec adopted;
        bool rejected = false;
        try {
            // Nonempty LoRA root requires JSON parsing of native metadata.
            adopted.adopt(source.release(), ".");
        } catch (const nlohmann::json::parse_error&) {
            rejected = true;
        }
        check_true(rejected);
        check_equal(adopted.count(), 2);
        check_equal(adopted[0].data[0], static_cast<uint8_t>(15));
        check_equal(adopted[1].data[0], static_cast<uint8_t>(16));
        adopted.clear();
        check_true(adopted.empty());
    }

    it("retires native video frames when metadata parsing throws before transfer")
    {
        auto* raw = static_cast<sd_video_generation_result_t*>(
            std::calloc(1, sizeof(sd_video_generation_result_t)));
        if (!raw) throw std::bad_alloc();
        std::unique_ptr<sd_video_generation_result_t, decltype(&free_sd_video_generation_result)>
            source(raw, &free_sd_video_generation_result);
        raw->frames = static_cast<sd_image_t*>(
            std::calloc(2, sizeof(sd_image_t)));
        if (!raw->frames) throw std::bad_alloc();
        raw->frame_count = 2;
        raw->frames[0] = small_image(5);
        raw->frames[1] = small_image(6);
        raw->generation_info = static_cast<char*>(std::malloc(2));
        if (!raw->generation_info) throw std::bad_alloc();
        std::memcpy(raw->generation_info, "!", 2);

        SDImageVec adopted;
        bool rejected = false;
        try {
            adopted.adopt_video(source.release(), ".");
        } catch (const nlohmann::json::parse_error&) {
            rejected = true;
        }
        check_true(rejected);
        check_true(adopted.empty());
    }

}
