/*
 * sal.h — minimal no-op shim for non-MSVC builds.
 *
 * DirectXMath.h unconditionally `#include "sal.h"` and expects the
 * Microsoft Source-code Annotation Language macros to exist. macOS /
 * Linux clang don't ship sal.h; this shim makes every SAL annotation
 * a no-op so the headers compile cleanly. Only the small subset
 * DirectXMath + DirectXTex BC6HBC7 actually reference is listed.
 */
#ifndef DXTEX_SHIM_SAL_H
#define DXTEX_SHIM_SAL_H

#define _In_
#define _In_opt_
#define _In_z_
#define _In_reads_(x)
#define _In_reads_bytes_(x)
#define _In_reads_or_z_(x)

#define _Out_
#define _Out_opt_
#define _Out_writes_(x)
#define _Out_writes_bytes_(x)
#define _Out_writes_to_(x, y)
#define _Out_writes_to_opt_(x, y)
#define _Out_writes_z_(x)
#define _Out_writes_all_(x)

#define _Inout_
#define _Inout_opt_
#define _Inout_updates_(x)
#define _Inout_updates_bytes_(x)
#define _Inout_updates_all_(x)

#define _When_(c, x)
#define _Pre_satisfies_(x)
#define _Post_satisfies_(x)

#define _Field_size_(x)
#define _Field_size_opt_(x)
#define _Field_size_bytes_(x)
#define _Field_size_bytes_full_(x)
#define _Field_size_full_(x)

#define _Use_decl_annotations_

#define _In_range_(min, max)
#define _Analysis_assume_(x) ((void)0)

#define _Maybenull_
#define _Notnull_
#define _Reserved_

#define _Success_(x)
#define _Check_return_

#endif /* DXTEX_SHIM_SAL_H */
