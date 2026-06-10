// liboyoltc consumers don't run user-facing strings through gettext, but
// libbitcoin_util references G_TRANSLATION_FUN as an extern global. The
// litecoin tool binaries (bitcoin-tx, bitcoin-cli, bitcoind, bitcoin-wallet)
// each define this as nullptr; we do the same for the lib. The header is
// included so the definition matches the declared external linkage —
// without it, `const T = ...;` is a file-local symbol in C++ and the
// linker still complains about an undefined external.
#include <util/translation.h>

const std::function<std::string(const char*)> G_TRANSLATION_FUN = nullptr;
