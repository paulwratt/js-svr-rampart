#include "rampart.h"
#include "include/version.h"
#include "rampart_timestamp.h"

void duk_rp_push_rampart_version(duk_context *ctx)
{
    if (!duk_get_global_string(ctx, "rampart"))
    {
        duk_pop(ctx);
        duk_push_object(ctx);
    }
    duk_push_sprintf(ctx, "%d.%d.%d%s+%s", RAMPART_VERSION_MAJOR, RAMPART_VERSION_MINOR, RAMPART_VERSION_PATCH, RAMPART_VERSION_PRERELEASE, RAMPART_VERSION_TIMESTAMP);
    duk_put_prop_string(ctx, -2, "versionBuild");
    duk_push_sprintf(ctx, "%d.%d.%d%s", RAMPART_VERSION_MAJOR, RAMPART_VERSION_MINOR, RAMPART_VERSION_PATCH, RAMPART_VERSION_PRERELEASE);
    duk_put_prop_string(ctx, -2, "version");
    duk_push_number(ctx, (double) RAMPART_VERSION_MAJOR*10000 +  RAMPART_VERSION_MINOR*100 + RAMPART_VERSION_PATCH);
    duk_put_prop_string(ctx, -2, "versionNumber");

    duk_eval_string(ctx, "rampart.utils.scanDate(\"" RAMPART_VERSION_TIMESTAMP "\",\"%Y-%m-%dT%H:%M:%SZ\");");
    duk_put_prop_string(ctx, -2, "versionDate");

    duk_put_global_string(ctx, "rampart");

}