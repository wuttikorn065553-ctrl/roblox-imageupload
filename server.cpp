#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <unordered_map>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <curl/curl.h>

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "third_party/stb/stb_image_resize.h"

struct ImageData {
    int width;
    int height;
    std::vector<std::vector<std::vector<uint8_t>>> pixels;
};

std::unordered_map<std::string, ImageData> CACHE;

// ================== NSFW FILTER ==================

bool containsNSFWKeyword(const std::string& url) {
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    return (lower.find("nsfw") != std::string::npos ||
            lower.find("porn") != std::string::npos ||
            lower.find("hentai") != std::string::npos);
}

bool isSkin(uint8_t r, uint8_t g, uint8_t b) {
    return (r > 95 && g > 40 && b > 20 &&
            (std::max({r,g,b}) - std::min({r,g,b})) > 15 &&
            abs(r - g) > 15 &&
            r > g && r > b);
}

bool isNSFWImage(const std::vector<unsigned char>& pixels, int totalPixels) {
    int skinCount = 0;

    for (int i = 0; i < totalPixels; i++) {
        int r = pixels[i*3];
        int g = pixels[i*3+1];
        int b = pixels[i*3+2];

        if (isSkin(r,g,b)) skinCount++;
    }

    float ratio = (float)skinCount / totalPixels;
    return ratio > 0.4f;
}

// =================================================

static size_t CurlWriteToFile(void* contents, size_t size, size_t nmemb, void* userp) {
    std::ofstream* ofs = static_cast<std::ofstream*>(userp);
    ofs->write(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

bool downloadImage(const std::string& url, const std::string& path) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) {
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ofs);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    ofs.close();

    return res == CURLE_OK;
}

// ================== CORE ==================

ImageData loadImageToMatrix(const std::string& url, int resize) {

    if (containsNSFWKeyword(url)) {
        throw std::runtime_error("NSFW keyword blocked");
    }

    // whitelist
    if (!(url.rfind("https://cdn.discordapp.com/", 0) == 0 ||
          url.rfind("https://media.discordapp.net/", 0) == 0)) {
        throw std::runtime_error("Only Discord links allowed");
    }

    std::string tmp = "/tmp/image.bin";

    if (!downloadImage(url, tmp)) {
        throw std::runtime_error("download failed");
    }

    int w, h, c;
    unsigned char* src = stbi_load(tmp.c_str(), &w, &h, &c, 3);
    if (!src) throw std::runtime_error("stbi failed");

    int target = std::max(8, std::min(128, resize));

    std::vector<unsigned char> pixels(target * target * 3);

    stbir_resize_uint8(src, w, h, 0,
        pixels.data(), target, target, 0, 3);

    stbi_image_free(src);

    // 🔥 NSFW IMAGE CHECK
    if (isNSFWImage(pixels, target * target)) {
        throw std::runtime_error("NSFW image blocked");
    }

    ImageData out;
    out.width = target;
    out.height = target;
    out.pixels.resize(target, std::vector<std::vector<uint8_t>>(target, std::vector<uint8_t>(3)));

    for (int y = 0; y < target; ++y) {
        for (int x = 0; x < target; ++x) {
            out.pixels[y][x][0] = pixels[(y * target + x) * 3 + 0];
            out.pixels[y][x][1] = pixels[(y * target + x) * 3 + 1];
            out.pixels[y][x][2] = pixels[(y * target + x) * 3 + 2];
        }
    }

    return out;
}

// ================== JSON ==================

std::string toJson(const ImageData& img) {
    std::ostringstream s;
    s << "{\"width\":" << img.width << ",\"height\":" << img.height << ",\"pixels\":[";
    for (int y=0; y<img.height; ++y) {
        s << "[";
        for (int x=0; x<img.width; ++x) {
            const auto &p = img.pixels[y][x];
            s << "[" << (int)p[0] << "," << (int)p[1] << "," << (int)p[2] << "]";
            if (x<img.width-1) s << ",";
        }
        s << "]";
        if (y<img.height-1) s << ",";
    }
    s << "]}";
    return s.str();
}

// ================== SERVER ==================

int main() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8787);

    bind(sockfd, (sockaddr*)&addr, sizeof(addr));
    listen(sockfd, 16);

    while (true) {
        int client = accept(sockfd, nullptr, nullptr);

        char buf[8192];
        int n = read(client, buf, sizeof(buf)-1);
        buf[n] = '\0';

        std::string req(buf);

        try {
            size_t pos = req.find("url=");
            if (pos == std::string::npos) throw std::runtime_error("no url");

            std::string url = req.substr(pos + 4);
            int resize = 64;

            std::string key = url + "_" + std::to_string(resize);

            ImageData img;

            if (CACHE.find(key) != CACHE.end()) {
                img = CACHE[key];
            } else {
                img = loadImageToMatrix(url, resize);
                CACHE[key] = img;
            }

            std::string body = toJson(img);

            std::ostringstream res;
            res << "HTTP/1.1 200 OK\r\n"
                << "Content-Type: application/json\r\n"
                << "Access-Control-Allow-Origin: *\r\n"
                << "Content-Length: " << body.size() << "\r\n\r\n"
                << body;

            write(client, res.str().c_str(), res.str().size());

        } catch (...) {
            std::string err = "{\"error\":\"blocked\"}";
            write(client, err.c_str(), err.size());
        }

        close(client);
    }
}