// Skip intros: the EA and Free Radical logo movies at boot are not played.
#include <cstring>
#include <string>

#include "common/options.h"
#include "xdk/hle.h"

// PlayMovie(name, flags), cdecl: plays d:\xmv\<name>.xmv to its end; the boot sequence passes eag_<language>, then frd
X86_EXTERN_FUNC(__imp__sub_00030280);
X86_FUNC(sub_00030280) {
    std::string name = kernel::ReadCString(kernel::R32(ctx.esp + 4));
    bool logo = name.rfind("eag_", 0) == 0 || name == "frd";
    if (logo && game::Opt().skip_intros) {
        kernel::Return(ctx, 0, 0);
        return;
    }
    __imp__sub_00030280(ctx, base);
}
