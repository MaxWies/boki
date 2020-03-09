#include "base/init.h"

#include <absl/flags/parse.h>
#include <absl/flags/flag.h>
#include <absl/debugging/symbolize.h>
#include <absl/debugging/failure_signal_handler.h>
#include <glog/logging.h>

ABSL_FLAG(int, glog_v, 0, "--v for glog library");

namespace faas {
namespace base {

void InitMain(int argc, char* argv[],
              std::vector<char*>* positional_args) {
    absl::InitializeSymbolizer(argv[0]);
    absl::FailureSignalHandlerOptions options;
    absl::InstallFailureSignalHandler(options);

    std::vector<char*> unparsed_args = absl::ParseCommandLine(argc, argv);

    FLAGS_logtostderr = true;
    FLAGS_v = absl::GetFlag(FLAGS_glog_v);
    google::InitGoogleLogging(argv[0]);

    if (positional_args == nullptr && unparsed_args.size() > 1) {
        LOG(FATAL) << "This program does not accept positional arguments";
    }
    if (positional_args != nullptr) {
        positional_args->clear();
        for (size_t i = 1; i < unparsed_args.size(); i++) {
            positional_args->push_back(unparsed_args[i]);
        }
    }
}

}  // namespace base
}  // namespace faas
