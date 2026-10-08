// The editor decodes thumbnails and downloaded previews itself (the engine's copy of stb_image is
// private to foh_engine.dll), so it carries its own instance, shared by the editor's translation units.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include <stb_image.h>
