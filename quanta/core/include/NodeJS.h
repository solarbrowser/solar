#ifndef QUANTA_NODEJS_H
#define QUANTA_NODEJS_H

#include "Value.h"
#include "Context.h"
#include <fstream>
#include <filesystem>
#include <thread>
#include <future>

namespace Quanta {

/**
 * Node.js compatibility layer
 * Provides file system, HTTP, and other Node.js APIs
 */
class NodeJS {
public:
    // File System API
    static Value fs_readFile(Context& ctx, const std::vector<Value>& args);
    static Value fs_writeFile(Context& ctx, const std::vector<Value>& args);
    static Value fs_appendFile(Context& ctx, const std::vector<Value>& args);
    static Value fs_exists(Context& ctx, const std::vector<Value>& args);
    static Value fs_mkdir(Context& ctx, const std::vector<Value>& args);
    static Value fs_rmdir(Context& ctx, const std::vector<Value>& args);
    static Value fs_unlink(Context& ctx, const std::vector<Value>& args);
    static Value fs_stat(Context& ctx, const std::vector<Value>& args);
    static Value fs_readdir(Context& ctx, const std::vector<Value>& args);
    
    // Synchronous versions
    static Value fs_readFileSync(Context& ctx, const std::vector<Value>& args);
    static Value fs_writeFileSync(Context& ctx, const std::vector<Value>& args);
    static Value fs_existsSync(Context& ctx, const std::vector<Value>& args);
    static Value fs_mkdirSync(Context& ctx, const std::vector<Value>& args);
    static Value fs_statSync(Context& ctx, const std::vector<Value>& args);
    static Value fs_readdirSync(Context& ctx, const std::vector<Value>& args);
    
    // Path API
    static Value path_join(Context& ctx, const std::vector<Value>& args);
    static Value path_resolve(Context& ctx, const std::vector<Value>& args);
    static Value path_dirname(Context& ctx, const std::vector<Value>& args);
    static Value path_basename(Context& ctx, const std::vector<Value>& args);
    static Value path_extname(Context& ctx, const std::vector<Value>& args);
    static Value path_normalize(Context& ctx, const std::vector<Value>& args);
    static Value path_isAbsolute(Context& ctx, const std::vector<Value>& args);
    
    // HTTP API (basic)
    static Value http_createServer(Context& ctx, const std::vector<Value>& args);
    static Value http_request(Context& ctx, const std::vector<Value>& args);
    static Value http_get(Context& ctx, const std::vector<Value>& args);
    
    // OS API
    static Value os_platform(Context& ctx, const std::vector<Value>& args);
    static Value os_arch(Context& ctx, const std::vector<Value>& args);
    static Value os_cpus(Context& ctx, const std::vector<Value>& args);
    static Value os_hostname(Context& ctx, const std::vector<Value>& args);
    static Value os_homedir(Context& ctx, const std::vector<Value>& args);
    static Value os_tmpdir(Context& ctx, const std::vector<Value>& args);
    
    // Process API
    static Value process_exit(Context& ctx, const std::vector<Value>& args);
    static Value process_cwd(Context& ctx, const std::vector<Value>& args);
    static Value process_chdir(Context& ctx, const std::vector<Value>& args);
    static Value process_env_get(Context& ctx, const std::vector<Value>& args);
    
    // Crypto API (basic)
    static Value crypto_randomBytes(Context& ctx, const std::vector<Value>& args);
    static Value crypto_createHash(Context& ctx, const std::vector<Value>& args);
    
    // Util API
    static Value util_format(Context& ctx, const std::vector<Value>& args);
    static Value util_inspect(Context& ctx, const std::vector<Value>& args);
    
    // Events API
    static Value events_EventEmitter(Context& ctx, const std::vector<Value>& args);
    
private:
    static std::string get_mime_type(const std::string& filename);
    static std::string get_current_directory();
    static bool is_safe_path(const std::string& path);
};

} // namespace Quanta

#endif // QUANTA_NODEJS_H