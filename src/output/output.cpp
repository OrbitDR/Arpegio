#include "output/output.h"

namespace arpegio {

std::unique_ptr<Output> make_asio_output(const DeviceInfo& device, const OutputOptions& options);
std::unique_ptr<Output> make_wasapi_output(const DeviceInfo& device, const OutputOptions& options);

std::unique_ptr<Output> make_output(const DeviceInfo& device, const OutputOptions& options) {
    return device.kind == OutputKind::Asio ? make_asio_output(device, options) : make_wasapi_output(device, options);
}

}  // namespace arpegio
