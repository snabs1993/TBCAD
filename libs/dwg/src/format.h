#pragma once

namespace tbcad::dwg::detail {

// Which object layout the file uses. Each flag includes the earlier ones.
struct Format {
    bool r2004 = false;  // ENC colours, no links, owned object counts
    bool r2007 = false;  // strings in a separate UTF-16 stream
    bool r2010 = false;  // handle stream size, new object type form
    bool r2013 = false;  // data store bit
    bool r2018 = false;  // attribute type
};

}  // namespace tbcad::dwg::detail
