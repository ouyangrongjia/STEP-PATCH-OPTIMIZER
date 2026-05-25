#include "io/StepWriter.h"

#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>

#include <string>

namespace spo {

namespace {

std::string pathToOcctString(const std::filesystem::path& path) {
    const auto utf8Path = path.u8string();
    return {reinterpret_cast<const char*>(utf8Path.c_str()), utf8Path.size()};
}

}

Result StepWriter::write(const ShapeDocument& document, const std::filesystem::path& path) const {
    if (!document.hasShape()) {
        return Result::error("当前没有已加载的模型。");
    }

    STEPControl_Writer writer;
    const auto transferStatus = writer.Transfer(document.shape(), STEPControl_AsIs);
    if (transferStatus != IFSelect_RetDone) {
        return Result::error("OCCT 为 STEP 导出转换模型失败。");
    }

    const auto occtPath = pathToOcctString(path);
    const auto writeStatus = writer.Write(occtPath.c_str());
    if (writeStatus != IFSelect_RetDone) {
        return Result::error("OCCT 写出 STEP 文件失败。");
    }

    return Result::ok();
}

}
