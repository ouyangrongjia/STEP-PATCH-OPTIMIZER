#include "io/StepReader.h"

#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Reader.hxx>

namespace spo {

Result StepReader::canRead(const std::filesystem::path& path) const {
    const auto ext = path.extension().string();
    if (ext == ".step" || ext == ".stp" || ext == ".STEP" || ext == ".STP") {
        return Result::ok();
    }
    return Result::error("需要选择 STEP 或 STP 文件。");
}

StepReadResult StepReader::read(const std::filesystem::path& path) const {
    const auto readable = canRead(path);
    if (!readable.success()) {
        return {readable, {}};
    }

    STEPControl_Reader reader;
    const auto status = reader.ReadFile(path.string().c_str());
    if (status != IFSelect_RetDone) {
        return {Result::error("OCCT 读取 STEP/STP 文件失败。"), {}};
    }

    const auto roots = reader.NbRootsForTransfer();
    if (roots <= 0) {
        return {Result::error("STEP/STP 文件没有可转换的根对象。"), {}};
    }

    reader.TransferRoots();
    auto shape = reader.OneShape();
    if (shape.IsNull()) {
        return {Result::error("STEP/STP 转换结果为空模型。"), {}};
    }

    return {Result::ok(), ShapeDocument(shape, path)};
}

}
