/** ==========================================================================
* 2015 by KjellKod.cc
*
* This code is PUBLIC DOMAIN to use at your own risk and comes
* with no warranties. This code is yours to share, use and modify with no
* strings attached and no restrictions or obligations.
* ============================================================================*
* PUBLIC DOMAIN and Not copywrited. First published at KjellKod.cc
* ********************************************* */

#pragma once


#include <string>
#include <vector>
#include <map>

namespace RotateTestHelper {
   std::string ReadContent(const std::string filename);
   std::string FlattenToString(const std::vector<std::string>& content);
   std::string FlattenToString(const std::map<long, std::string>& content);
   bool Exists(const std::string content, const std::string expected);
}
