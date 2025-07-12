#include "CSSParser.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>

namespace CSS3Parser {

// Named color constants
static const std::unordered_map<std::string, CSSColor> named_colors = {
    {"transparent", CSSColor(CSSColor::Transparent, 0, 0, 0, 0)},
    {"black", CSSColor(CSSColor::RGB, 0, 0, 0, 1)},
    {"white", CSSColor(CSSColor::RGB, 255, 255, 255, 1)},
    {"red", CSSColor(CSSColor::RGB, 255, 0, 0, 1)},
    {"green", CSSColor(CSSColor::RGB, 0, 128, 0, 1)},
    {"blue", CSSColor(CSSColor::RGB, 0, 0, 255, 1)},
    {"yellow", CSSColor(CSSColor::RGB, 255, 255, 0, 1)},
    {"cyan", CSSColor(CSSColor::RGB, 0, 255, 255, 1)},
    {"magenta", CSSColor(CSSColor::RGB, 255, 0, 255, 1)},
    {"silver", CSSColor(CSSColor::RGB, 192, 192, 192, 1)},
    {"gray", CSSColor(CSSColor::RGB, 128, 128, 128, 1)},
    {"maroon", CSSColor(CSSColor::RGB, 128, 0, 0, 1)},
    {"olive", CSSColor(CSSColor::RGB, 128, 128, 0, 1)},
    {"lime", CSSColor(CSSColor::RGB, 0, 255, 0, 1)},
    {"aqua", CSSColor(CSSColor::RGB, 0, 255, 255, 1)},
    {"teal", CSSColor(CSSColor::RGB, 0, 128, 128, 1)},
    {"navy", CSSColor(CSSColor::RGB, 0, 0, 128, 1)},
    {"fuchsia", CSSColor(CSSColor::RGB, 255, 0, 255, 1)},
    {"purple", CSSColor(CSSColor::RGB, 128, 0, 128, 1)},
    // CSS3 extended colors
    {"aliceblue", CSSColor(CSSColor::RGB, 240, 248, 255, 1)},
    {"antiquewhite", CSSColor(CSSColor::RGB, 250, 235, 215, 1)},
    {"aquamarine", CSSColor(CSSColor::RGB, 127, 255, 212, 1)},
    {"azure", CSSColor(CSSColor::RGB, 240, 255, 255, 1)},
    {"beige", CSSColor(CSSColor::RGB, 245, 245, 220, 1)},
    {"bisque", CSSColor(CSSColor::RGB, 255, 228, 196, 1)},
    {"blanchedalmond", CSSColor(CSSColor::RGB, 255, 235, 205, 1)},
    {"blueviolet", CSSColor(CSSColor::RGB, 138, 43, 226, 1)},
    {"brown", CSSColor(CSSColor::RGB, 165, 42, 42, 1)},
    {"burlywood", CSSColor(CSSColor::RGB, 222, 184, 135, 1)},
    {"cadetblue", CSSColor(CSSColor::RGB, 95, 158, 160, 1)},
    {"chartreuse", CSSColor(CSSColor::RGB, 127, 255, 0, 1)},
    {"chocolate", CSSColor(CSSColor::RGB, 210, 105, 30, 1)},
    {"coral", CSSColor(CSSColor::RGB, 255, 127, 80, 1)},
    {"cornflowerblue", CSSColor(CSSColor::RGB, 100, 149, 237, 1)},
    {"cornsilk", CSSColor(CSSColor::RGB, 255, 248, 220, 1)},
    {"crimson", CSSColor(CSSColor::RGB, 220, 20, 60, 1)},
    {"darkblue", CSSColor(CSSColor::RGB, 0, 0, 139, 1)},
    {"darkcyan", CSSColor(CSSColor::RGB, 0, 139, 139, 1)},
    {"darkgoldenrod", CSSColor(CSSColor::RGB, 184, 134, 11, 1)},
    {"darkgray", CSSColor(CSSColor::RGB, 169, 169, 169, 1)},
    {"darkgreen", CSSColor(CSSColor::RGB, 0, 100, 0, 1)},
    {"darkkhaki", CSSColor(CSSColor::RGB, 189, 183, 107, 1)},
    {"darkmagenta", CSSColor(CSSColor::RGB, 139, 0, 139, 1)},
    {"darkolivegreen", CSSColor(CSSColor::RGB, 85, 107, 47, 1)},
    {"darkorange", CSSColor(CSSColor::RGB, 255, 140, 0, 1)},
    {"darkorchid", CSSColor(CSSColor::RGB, 153, 50, 204, 1)},
    {"darkred", CSSColor(CSSColor::RGB, 139, 0, 0, 1)},
    {"darksalmon", CSSColor(CSSColor::RGB, 233, 150, 122, 1)},
    {"darkseagreen", CSSColor(CSSColor::RGB, 143, 188, 143, 1)},
    {"darkslateblue", CSSColor(CSSColor::RGB, 72, 61, 139, 1)},
    {"darkslategray", CSSColor(CSSColor::RGB, 47, 79, 79, 1)},
    {"darkturquoise", CSSColor(CSSColor::RGB, 0, 206, 209, 1)},
    {"darkviolet", CSSColor(CSSColor::RGB, 148, 0, 211, 1)},
    {"deeppink", CSSColor(CSSColor::RGB, 255, 20, 147, 1)},
    {"deepskyblue", CSSColor(CSSColor::RGB, 0, 191, 255, 1)},
    {"dimgray", CSSColor(CSSColor::RGB, 105, 105, 105, 1)},
    {"dodgerblue", CSSColor(CSSColor::RGB, 30, 144, 255, 1)},
    {"firebrick", CSSColor(CSSColor::RGB, 178, 34, 34, 1)},
    {"floralwhite", CSSColor(CSSColor::RGB, 255, 250, 240, 1)},
    {"forestgreen", CSSColor(CSSColor::RGB, 34, 139, 34, 1)},
    {"gainsboro", CSSColor(CSSColor::RGB, 220, 220, 220, 1)},
    {"ghostwhite", CSSColor(CSSColor::RGB, 248, 248, 255, 1)},
    {"gold", CSSColor(CSSColor::RGB, 255, 215, 0, 1)},
    {"goldenrod", CSSColor(CSSColor::RGB, 218, 165, 32, 1)},
    {"greenyellow", CSSColor(CSSColor::RGB, 173, 255, 47, 1)},
    {"honeydew", CSSColor(CSSColor::RGB, 240, 255, 240, 1)},
    {"hotpink", CSSColor(CSSColor::RGB, 255, 105, 180, 1)},
    {"indianred", CSSColor(CSSColor::RGB, 205, 92, 92, 1)},
    {"indigo", CSSColor(CSSColor::RGB, 75, 0, 130, 1)},
    {"ivory", CSSColor(CSSColor::RGB, 255, 255, 240, 1)},
    {"khaki", CSSColor(CSSColor::RGB, 240, 230, 140, 1)},
    {"lavender", CSSColor(CSSColor::RGB, 230, 230, 250, 1)},
    {"lavenderblush", CSSColor(CSSColor::RGB, 255, 240, 245, 1)},
    {"lawngreen", CSSColor(CSSColor::RGB, 124, 252, 0, 1)},
    {"lemonchiffon", CSSColor(CSSColor::RGB, 255, 250, 205, 1)},
    {"lightblue", CSSColor(CSSColor::RGB, 173, 216, 230, 1)},
    {"lightcoral", CSSColor(CSSColor::RGB, 240, 128, 128, 1)},
    {"lightcyan", CSSColor(CSSColor::RGB, 224, 255, 255, 1)},
    {"lightgoldenrodyellow", CSSColor(CSSColor::RGB, 250, 250, 210, 1)},
    {"lightgray", CSSColor(CSSColor::RGB, 211, 211, 211, 1)},
    {"lightgreen", CSSColor(CSSColor::RGB, 144, 238, 144, 1)},
    {"lightpink", CSSColor(CSSColor::RGB, 255, 182, 193, 1)},
    {"lightsalmon", CSSColor(CSSColor::RGB, 255, 160, 122, 1)},
    {"lightseagreen", CSSColor(CSSColor::RGB, 32, 178, 170, 1)},
    {"lightskyblue", CSSColor(CSSColor::RGB, 135, 206, 250, 1)},
    {"lightslategray", CSSColor(CSSColor::RGB, 119, 136, 153, 1)},
    {"lightsteelblue", CSSColor(CSSColor::RGB, 176, 196, 222, 1)},
    {"lightyellow", CSSColor(CSSColor::RGB, 255, 255, 224, 1)},
    {"limegreen", CSSColor(CSSColor::RGB, 50, 205, 50, 1)},
    {"linen", CSSColor(CSSColor::RGB, 250, 240, 230, 1)},
    {"mediumaquamarine", CSSColor(CSSColor::RGB, 102, 205, 170, 1)},
    {"mediumblue", CSSColor(CSSColor::RGB, 0, 0, 205, 1)},
    {"mediumorchid", CSSColor(CSSColor::RGB, 186, 85, 211, 1)},
    {"mediumpurple", CSSColor(CSSColor::RGB, 147, 112, 219, 1)},
    {"mediumseagreen", CSSColor(CSSColor::RGB, 60, 179, 113, 1)},
    {"mediumslateblue", CSSColor(CSSColor::RGB, 123, 104, 238, 1)},
    {"mediumspringgreen", CSSColor(CSSColor::RGB, 0, 250, 154, 1)},
    {"mediumturquoise", CSSColor(CSSColor::RGB, 72, 209, 204, 1)},
    {"mediumvioletred", CSSColor(CSSColor::RGB, 199, 21, 133, 1)},
    {"midnightblue", CSSColor(CSSColor::RGB, 25, 25, 112, 1)},
    {"mintcream", CSSColor(CSSColor::RGB, 245, 255, 250, 1)},
    {"mistyrose", CSSColor(CSSColor::RGB, 255, 228, 225, 1)},
    {"moccasin", CSSColor(CSSColor::RGB, 255, 228, 181, 1)},
    {"navajowhite", CSSColor(CSSColor::RGB, 255, 222, 173, 1)},
    {"oldlace", CSSColor(CSSColor::RGB, 253, 245, 230, 1)},
    {"olivedrab", CSSColor(CSSColor::RGB, 107, 142, 35, 1)},
    {"orange", CSSColor(CSSColor::RGB, 255, 165, 0, 1)},
    {"orangered", CSSColor(CSSColor::RGB, 255, 69, 0, 1)},
    {"orchid", CSSColor(CSSColor::RGB, 218, 112, 214, 1)},
    {"palegoldenrod", CSSColor(CSSColor::RGB, 238, 232, 170, 1)},
    {"palegreen", CSSColor(CSSColor::RGB, 152, 251, 152, 1)},
    {"paleturquoise", CSSColor(CSSColor::RGB, 175, 238, 238, 1)},
    {"palevioletred", CSSColor(CSSColor::RGB, 219, 112, 147, 1)},
    {"papayawhip", CSSColor(CSSColor::RGB, 255, 239, 213, 1)},
    {"peachpuff", CSSColor(CSSColor::RGB, 255, 218, 185, 1)},
    {"peru", CSSColor(CSSColor::RGB, 205, 133, 63, 1)},
    {"pink", CSSColor(CSSColor::RGB, 255, 192, 203, 1)},
    {"plum", CSSColor(CSSColor::RGB, 221, 160, 221, 1)},
    {"powderblue", CSSColor(CSSColor::RGB, 176, 224, 230, 1)},
    {"rosybrown", CSSColor(CSSColor::RGB, 188, 143, 143, 1)},
    {"royalblue", CSSColor(CSSColor::RGB, 65, 105, 225, 1)},
    {"saddlebrown", CSSColor(CSSColor::RGB, 139, 69, 19, 1)},
    {"salmon", CSSColor(CSSColor::RGB, 250, 128, 114, 1)},
    {"sandybrown", CSSColor(CSSColor::RGB, 244, 164, 96, 1)},
    {"seagreen", CSSColor(CSSColor::RGB, 46, 139, 87, 1)},
    {"seashell", CSSColor(CSSColor::RGB, 255, 245, 238, 1)},
    {"sienna", CSSColor(CSSColor::RGB, 160, 82, 45, 1)},
    {"skyblue", CSSColor(CSSColor::RGB, 135, 206, 235, 1)},
    {"slateblue", CSSColor(CSSColor::RGB, 106, 90, 205, 1)},
    {"slategray", CSSColor(CSSColor::RGB, 112, 128, 144, 1)},
    {"snow", CSSColor(CSSColor::RGB, 255, 250, 250, 1)},
    {"springgreen", CSSColor(CSSColor::RGB, 0, 255, 127, 1)},
    {"steelblue", CSSColor(CSSColor::RGB, 70, 130, 180, 1)},
    {"tan", CSSColor(CSSColor::RGB, 210, 180, 140, 1)},
    {"thistle", CSSColor(CSSColor::RGB, 216, 191, 216, 1)},
    {"tomato", CSSColor(CSSColor::RGB, 255, 99, 71, 1)},
    {"turquoise", CSSColor(CSSColor::RGB, 64, 224, 208, 1)},
    {"violet", CSSColor(CSSColor::RGB, 238, 130, 238, 1)},
    {"wheat", CSSColor(CSSColor::RGB, 245, 222, 179, 1)},
    {"whitesmoke", CSSColor(CSSColor::RGB, 245, 245, 245, 1)},
    {"yellowgreen", CSSColor(CSSColor::RGB, 154, 205, 50, 1)}
};

// CSSColor implementation
std::string CSSColor::to_string() const {
    std::ostringstream ss;
    
    switch (type) {
        case RGB:
            if (values[3] == 1.0) {
                ss << "rgb(" << static_cast<int>(values[0]) << ", " 
                   << static_cast<int>(values[1]) << ", " 
                   << static_cast<int>(values[2]) << ")";
            } else {
                ss << "rgba(" << static_cast<int>(values[0]) << ", " 
                   << static_cast<int>(values[1]) << ", " 
                   << static_cast<int>(values[2]) << ", " 
                   << std::fixed << std::setprecision(3) << values[3] << ")";
            }
            break;
            
        case HSL:
            if (values[3] == 1.0) {
                ss << "hsl(" << static_cast<int>(values[0]) << ", " 
                   << static_cast<int>(values[1]) << "%, " 
                   << static_cast<int>(values[2]) << "%)";
            } else {
                ss << "hsla(" << static_cast<int>(values[0]) << ", " 
                   << static_cast<int>(values[1]) << "%, " 
                   << static_cast<int>(values[2]) << "%, " 
                   << std::fixed << std::setprecision(3) << values[3] << ")";
            }
            break;
            
        case HWB:
            ss << "hwb(" << static_cast<int>(values[0]) << " " 
               << static_cast<int>(values[1]) << "% " 
               << static_cast<int>(values[2]) << "%";
            if (values[3] != 1.0) {
                ss << " / " << std::fixed << std::setprecision(3) << values[3];
            }
            ss << ")";
            break;
            
        case LAB:
            ss << "lab(" << std::fixed << std::setprecision(2) << values[0] << "% " 
               << values[1] << " " << values[2];
            if (values[3] != 1.0) {
                ss << " / " << std::setprecision(3) << values[3];
            }
            ss << ")";
            break;
            
        case LCH:
            ss << "lch(" << std::fixed << std::setprecision(2) << values[0] << "% " 
               << values[1] << " " << values[2];
            if (values[3] != 1.0) {
                ss << " / " << std::setprecision(3) << values[3];
            }
            ss << ")";
            break;
            
        case Named:
            ss << name;
            break;
            
        case Hex:
            ss << "#" << std::hex << std::setfill('0');
            if (values[3] == 1.0) {
                ss << std::setw(2) << static_cast<int>(values[0])
                   << std::setw(2) << static_cast<int>(values[1])
                   << std::setw(2) << static_cast<int>(values[2]);
            } else {
                ss << std::setw(2) << static_cast<int>(values[0])
                   << std::setw(2) << static_cast<int>(values[1])
                   << std::setw(2) << static_cast<int>(values[2])
                   << std::setw(2) << static_cast<int>(values[3] * 255);
            }
            break;
            
        case Current:
            ss << "currentcolor";
            break;
            
        case Transparent:
            ss << "transparent";
            break;
    }
    
    return ss.str();
}

CSSColor CSSColor::from_hex(const std::string& hex) {
    std::string color_hex = hex;
    if (color_hex[0] == '#') {
        color_hex = color_hex.substr(1);
    }
    
    CSSColor color(Hex, 0, 0, 0, 1);
    
    if (color_hex.length() == 3) {
        // Short hex format #rgb -> #rrggbb
        color.values[0] = std::stoi(std::string(2, color_hex[0]), nullptr, 16);
        color.values[1] = std::stoi(std::string(2, color_hex[1]), nullptr, 16);
        color.values[2] = std::stoi(std::string(2, color_hex[2]), nullptr, 16);
    } else if (color_hex.length() == 4) {
        // Short hex with alpha #rgba -> #rrggbbaa
        color.values[0] = std::stoi(std::string(2, color_hex[0]), nullptr, 16);
        color.values[1] = std::stoi(std::string(2, color_hex[1]), nullptr, 16);
        color.values[2] = std::stoi(std::string(2, color_hex[2]), nullptr, 16);
        color.values[3] = std::stoi(std::string(2, color_hex[3]), nullptr, 16) / 255.0;
    } else if (color_hex.length() == 6) {
        // Full hex format #rrggbb
        color.values[0] = std::stoi(color_hex.substr(0, 2), nullptr, 16);
        color.values[1] = std::stoi(color_hex.substr(2, 2), nullptr, 16);
        color.values[2] = std::stoi(color_hex.substr(4, 2), nullptr, 16);
    } else if (color_hex.length() == 8) {
        // Full hex with alpha #rrggbbaa
        color.values[0] = std::stoi(color_hex.substr(0, 2), nullptr, 16);
        color.values[1] = std::stoi(color_hex.substr(2, 2), nullptr, 16);
        color.values[2] = std::stoi(color_hex.substr(4, 2), nullptr, 16);
        color.values[3] = std::stoi(color_hex.substr(6, 2), nullptr, 16) / 255.0;
    }
    
    return color;
}

CSSColor CSSColor::from_name(const std::string& name) {
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    
    auto it = named_colors.find(lower_name);
    if (it != named_colors.end()) {
        CSSColor color = it->second;
        color.name = name;
        return color;
    }
    
    // Return transparent if name not found
    CSSColor color(Named, 0, 0, 0, 0);
    color.name = name;
    return color;
}

// CSSValue implementation
std::string CSSValue::to_string() const {
    std::ostringstream ss;
    
    switch (type) {
        case ValueType::Keyword:
            ss << string_value;
            break;
            
        case ValueType::Number:
            ss << numeric_value;
            break;
            
        case ValueType::Percentage:
            ss << numeric_value << "%";
            break;
            
        case ValueType::Length:
        case ValueType::Angle:
        case ValueType::Time:
        case ValueType::Frequency:
        case ValueType::Resolution:
            ss << numeric_value << unit;
            break;
            
        case ValueType::Color:
            ss << color_value.to_string();
            break;
            
        case ValueType::String:
            ss << "\"" << string_value << "\"";
            break;
            
        case ValueType::Url:
            ss << "url(" << string_value << ")";
            break;
            
        case ValueType::Function: {
            ss << string_value << "(";
            bool first = true;
            for (const auto& [key, value] : function_args) {
                if (!first) ss << ", ";
                if (!key.empty()) ss << key << ": ";
                ss << value.to_string();
                first = false;
            }
            ss << ")";
            break;
        }
            
        case ValueType::List:
            for (size_t i = 0; i < list_values.size(); ++i) {
                if (i > 0) ss << " ";
                ss << list_values[i].to_string();
            }
            break;
            
        case ValueType::Custom:
            ss << string_value;
            break;
    }
    
    return ss.str();
}

} // namespace CSS3Parser