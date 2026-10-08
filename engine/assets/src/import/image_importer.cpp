#include "foh/asset_import.h"
#include "import_util.h"

namespace foh {

SourceFormat DetectSourceFormat(const std::filesystem::path& path) {
    const std::string ext = import_util::lowerExtension(path);
    if (ext == ".gltf" || ext == ".glb") return SourceFormat::Gltf;
    if (ext == ".fbx") return SourceFormat::Fbx;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") return SourceFormat::Image;
    return SourceFormat::Unknown;
}

bool ImportImageFile(const std::filesystem::path& path, ImportedScene& out, std::string& error) {
    std::vector<u8> bytes;
    if (!ReadFileBytes(path, bytes)) {
        error = "cannot read " + path.string();
        return false;
    }
    ImportedImage image;
    if (!import_util::decodeImage(bytes.data(), bytes.size(), image, error)) return false;
    image.name = path.stem().string();
    image.key = image.name;
    ImportedScene scene;
    scene.name = image.name;
    scene.images.push_back(std::move(image));
    out = std::move(scene);
    return true;
}

} // namespace foh
