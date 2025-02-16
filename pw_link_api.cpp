// gcc pw-tut2.c -o tut2 -I /usr/include/spa-0.2 -I /usr/include/pipewire-0.3 -D_REENTRANT -lpipewire-0.3
#include <pipewire/pipewire.h>
#include <signal.h>
#include <thread>
#include <memory>
#include <functional>
#include <iostream>
#include <mutex>
#include <map>
#include <set>
#include <algorithm>

#include <pistache/endpoint.h>
#include <pistache/http.h>
#include <pistache/router.h>
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/ostreamwrapper.h"

namespace lg {
    struct Globals {
        Globals() {
        }
        static Globals & Instance() {
            static Globals myself;
            myself.dump.setstate(std::ios_base::badbit);
            return myself;
        }
        std::ostream & get_real() {
            return std::clog;
        }
        std::ostream & get_fake() {
            return dump;
        }
        int current_level = 7;
        std::mutex mutex;
        std::ostringstream dump;
    };
    template <int level, char... prefx>
    class Entry {
    public:
        Entry()
            : _g(Globals::Instance())
            , _use_real(_g.current_level >= level)
            , _os(_use_real ? _g.get_real(): _g.get_fake())
        {            
            if (_use_real) {
                _g.mutex.lock();
                char p[] = { '<','0'+level,'>', prefx... , ':', ' ', 0 };
                _os << p;
            }
        }
        ~Entry() {
            if (_use_real) {
                _os << std::endl;
                _g.mutex.unlock();
            }
        }
        template <typename T>
        std::ostream & operator << (T const & v) { return _os << v; }
        std::ostream & operator << (std::ostream & (fn)( std::ostream & )){ return _os << fn; }

    private:
        Globals & _g;
        bool _use_real;
        std::ostream & _os;
    };

    using Error = Entry<3, 'E','r','r','o','r'>;
    using Warn = Entry<4, 'W','a','r','n','i','n','g'>;
    using Notice = Entry<5, 'N','o','t','i','c','e'>;
    using Info = Entry<6, 'I','n','f','o'>;
    using Debug = Entry<7, 'D','e','b','u','g'>;
};

static std::function<void()> quit;
static void handle_sig(int sig) {
    if (quit) {
        quit();
    } else {
        std::cerr << "Got signal " << sig << " no exit action defined" << std::endl;
        exit(1);
    }
}

class PwWrapper {
        using obj_t = std::map<std::string, std::string>;
        using port_map_t = std::multimap<std::string, uint32_t>;
        using link_map_t = std::map<std::pair<uint32_t, uint32_t>, uint32_t>;
        using link_pending_list_t = std::list<std::tuple<uint32_t, uint32_t, bool>>;
        using link_name_pair_t = std::pair<std::string, std::string>;
        using link_name_set_t = std::set<link_name_pair_t>;

    public:
        using link_name_list_t = std::vector<link_name_pair_t>;
        PwWrapper()
        {
            init();
            std::thread tmp(std::bind(&PwWrapper::run, this));
            thread.swap(tmp);
        }

        ~PwWrapper() {
            lg::Debug() << "PW: Quit";
            quit = true;
            poke();
            lg::Debug() << "PW: join...";
            thread.join();
            lg::Debug() << "done";
        }

        auto get_port_names() {
            std::scoped_lock l(m);
            std::pair<std::vector<std::string>, std::vector<std::string>> result;
            for (auto & p: src_port_map)
                result.first.push_back(p.first);
            for (auto & p: dst_port_map)
                result.second.push_back(p.first);
            return result;
        }

        auto get_active_links() {
            std::scoped_lock l(m);
            link_name_list_t result;
            for (auto & link: link_map)
                result.push_back(std::make_pair(port_name(link.first.first), port_name(link.first.second)));
            return result;
        }

        auto get_desired_links() {
            link_name_list_t result;
            std::scoped_lock l(m);
            for (auto & p: desired_links)
                result.push_back(p);
            return result;
        }

        void remove_desired_links(link_name_list_t const & remove_list) {
            std::scoped_lock l(m);
            remove_desired_links_impl(remove_list);
            poke();
        }

        void add_desired_links(link_name_list_t const & add_list) {
            std::scoped_lock l(m);
            add_desired_links_impl(add_list);
            poke();
        }

        void set_desired_links(link_name_list_t const & new_desired_list) {
            std::scoped_lock l(m);
            link_name_list_t removed;
            std::set_difference(desired_links.begin(), desired_links.end(), new_desired_list.begin(), new_desired_list.end(), std::back_inserter(removed));
            remove_desired_links_impl(removed);
            add_desired_links_impl(new_desired_list);
            poke();
        }

        void show_ports() {
            std::scoped_lock l(m);
            lg::Debug() << "Sources:";
            for(auto & p : src_port_map) {
                lg::Debug() << "  " << p.first << ": " << p.second;
            }
            lg::Debug() << "Destinations:";
            for(auto & p : dst_port_map) {
                lg::Debug() << "  " << p.first << ": " << p.second;
            }
            lg::Debug() << "Links:";
            for(auto & link : link_map) {
                lg::Debug() << "  " << port_name(link.first.first) << " -> " << port_name(link.first.second) << " (" << link.second <<")";
            }
        }

        bool is_busy() {
            std::scoped_lock l(m);
            return busy;
        }

    private:
        void process_new() {
            std::scoped_lock l(m);
            if (!busy) {
                process_pending();
            } else {
                lg::Debug() << "New request while busy, waiting and hopin'";
            }
        }

        void poke() {
            pw_loop_invoke(
                pw_main_loop_get_loop(loop),
                [](struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data) {
                    reinterpret_cast<PwWrapper*>(user_data)->process_new();
                    return 0;
                },
                0, 0, 0, false, this
            );
        }

        void remove_desired_links_impl(link_name_list_t const & remove_list)
        {
            for (auto & del: remove_list) {
                desired_links.erase(del);
                auto srcidit = src_port_map.find(del.first);
                auto dstidit = dst_port_map.find(del.second);
                if (srcidit == src_port_map.end() || dstidit == dst_port_map.end()) {
                    lg::Debug() << "Unable to unlink ports " << del.first << " and " << del.second << ", not found";
                } else {
                    pending_links.push_back(std::make_tuple(srcidit->second, dstidit->second, false));
                }
            }
        }

        void add_desired_links_impl(link_name_list_t const & add_list)
        {
            for (auto & p: add_list) {
                auto ins = desired_links.insert(p);
                if (ins.second) {
                    auto srcidit = src_port_map.find(p.first);
                    auto dstidit = dst_port_map.find(p.second);
                    if (srcidit == src_port_map.end() || dstidit == dst_port_map.end()) {
                        lg::Debug() << "Unable to link ports " << p.first << " and " << p.second << ", not found";
                    } else {
                        pending_links.push_back(std::make_tuple(srcidit->second, dstidit->second, true));
                    }
                }
            }
        }

        static bool is_port(obj_t const &obj) { return obj.at("type") == "PipeWire:Interface:Port"; }
        static bool is_src(obj_t const &obj) { return obj.at("port.direction") == "out"; }
        static bool is_link(obj_t const &obj) { return obj.at("type") == "PipeWire:Interface:Link"; }
        static auto link_key(obj_t const &obj) { return std::make_pair(std::stoul(obj.at("link.output.port")), std::stoul(obj.at("link.input.port"))); }

        // Given a port object, returns the name we will use for referencing that port, this is based on the port and parent node name
        // rather than path or alias as these not specific enough with more than one of the same interface
        std::string port_name(obj_t const &obj) const {
            uint32_t node_idx = std::stoul(obj.at("node.id"));
            std::string node_name = objects.at(node_idx).at("node.name");
            return  node_name + ':' + obj.at("port.name");
        }
        // Same but start with the object index
        std::string port_name(uint32_t obj_id) const { return port_name(objects.at(obj_id)); }

        // Dump all properties of an object to stdout, starting with the type
        static void dump_obj(obj_t const &obj, std::string sep=" ") {
            lg::Debug() << obj.at("type");
            for(auto & p : obj)
                if (p.first != "type")
                    lg::Debug() << sep << p.first << "=" << p.second;
        }
        // As above but specify by and prefix with object id
        void dump_obj(uint32_t id) const {
            lg::Debug() << id << " ";
            dump_obj(objects.at(id), "\n  ");
            lg::Debug();
        }
        // As above, but add specified prefix
        void dump_obj(std::string pre, uint32_t id) const {
            lg::Debug() << pre << ": ";
            dump_obj(id);
        }
        // Called when a new object appears in the registry, updates our local object and port structures
        void add_object(uint32_t id, obj_t obj)
        {
            std::scoped_lock l(m);
            objects[id] = obj;
            dump_obj("Add", id);
            if (is_port(obj)) {
                bool issrc = is_src(obj);
                port_map_t & pm = issrc ? src_port_map : dst_port_map;
                auto name = port_name(obj);
                pm.insert(std::make_pair(name, id));
                // Eww linear search, but can't be arsed
                if (issrc) {
                    for (auto & link : desired_links) {
                        if (link.first == name) {
                            auto it = dst_port_map.find(link.second);
                            if (it != dst_port_map.end()) {
                                lg::Info() << "New source port, " << name << " will be linked link to " << link.second;
                                pending_links.push_back(std::make_tuple(id, it->second, true));
                            }
                        }
                    }
                } else {
                    for (auto & link : desired_links) {
                        if (link.second == name) {
                            auto it = src_port_map.find(link.second);
                            if (it != src_port_map.end()) {
                                lg::Info() << "New dest port, " << name << " will be linked to " << link.second;
                                pending_links.push_back(std::make_tuple(it->second, id, true));
                            }
                        }
                    }
                }
                if (!pending_links.empty() && !busy) {
                    lg::Debug() << "Triggering processing";
                    pw_core_sync(core, PW_ID_CORE, 0);
                }
            } else if (is_link(obj)) {
                link_map[link_key(obj)] = id;
            }
        }
        // Called when object disappears from the registry
        void remove_object(uint32_t id)
        {
            std::scoped_lock l(m);
            dump_obj("Del", id);
            auto it = objects.find(id);
            if (it == objects.end()){
                lg::Warn() << "Cannot delete object, not found " << id;
                return;
            }
            if (is_port(it->second)) {
                port_map_t & pm = is_src(it->second) ? src_port_map : dst_port_map;
                // What a palaver. Very annoying pw doesn't generate unique names
                auto itpair = pm.equal_range(port_name(it->second));
                for (auto eraseit = itpair.first; eraseit != itpair.second; ++eraseit)
                    if (eraseit->second == id) {
                        pm.erase(eraseit);
                        break;
                    }
            } else if (is_link(it->second)) {
                link_map.erase(link_key(it->second));
                auto key = link_key(it->second);
                link_map[key] = id;
                auto it = desired_links.find(std::make_pair(port_name(key.first), port_name(key.second)));
                if (it != desired_links.end()) {
                    lg::Info() << "Removed link from " << it->first << " to " << it->second << " is desired, reinstating";
                    pending_links.push_back(std::make_tuple(key.first, key.second, true));
                    if (!busy) {
                        lg::Debug() << "Triggering processing";
                        pw_core_sync(core, PW_ID_CORE, 0);
                    }
                }
            }
            objects.erase(it);
        }

        bool make_link_impl(uint32_t src, uint32_t dst) {
            auto srcit = objects.find(src);
            auto dstit = objects.find(dst);
            if (srcit == objects.end() || dstit == objects.end()) {
                lg::Warn() << "Unable to link ports " << src << " and " << dst << ", not found";
                return false;
            }
            if (link_proxy) {
                lg::Error() << "Cannot make link: link_proxy already allocated! (" << link_proxy << ")";
                return false;
            }

            struct pw_properties *props = pw_properties_new(
                PW_KEY_LINK_OUTPUT_NODE, srcit->second.at("node.id"),
                PW_KEY_LINK_OUTPUT_PORT, std::to_string(src).c_str(),
                PW_KEY_LINK_INPUT_NODE, dstit->second.at("node.id"),
                PW_KEY_LINK_INPUT_PORT, std::to_string(dst).c_str(),
                PW_KEY_OBJECT_LINGER, "true",
                NULL);

            if (props == 0) {
                lg::Error() << "Unable to link ports " << src << " and " << dst << ", unable to create properties";
                return false;
            }

            //struct pw_proxy *
            link_proxy = (struct pw_proxy*)pw_core_create_object(
                core,
                "link-factory",
                PW_TYPE_INTERFACE_Link,
                PW_VERSION_LINK,
                &props->dict,
                0);

            lg::Debug() << "link_proxy = " << link_proxy << ". Adding listener/Linking ports " << src << " and " << dst;
        
        	pw_proxy_add_listener(link_proxy, &link_proxy_listener, &link_proxy_events, this);

            pw_proxy_sync(link_proxy, 42);

            // Need this?
            pw_properties_free(props);
            return true;
        }

        void init()
        {
            pw_init(0, 0);

            loop = pw_main_loop_new(NULL /* properties */);
            context = pw_context_new(pw_main_loop_get_loop(loop),
                            NULL /* properties */,
                            0 /* user_data size */);

            core = pw_context_connect(context,
                            NULL /* properties */,
                            0 /* user_data size */);

            registry = pw_core_get_registry(core, PW_VERSION_REGISTRY,
                            0 /* user_data size */);

            pw_registry_add_listener(registry, &registry_listener,
                                            &registry_events, this);

            pw_core_add_listener(core, &core_listener, &core_events, this);

            lg::Debug() << "PW Wrapper: " << this << " Initialized loop: " << loop << " context: " << context << " core: " << core << " registry: " << registry;
        }

        void process_pending()
        {
            if (link_proxy) {
                // Hopefully we've just been poked while still processing the last request in which case everything is fine
                lg::Warn() << "Cannot process pending list: link_proxy not null (" << link_proxy << ") while " << (busy ? "busy" : "not busy");
                return;
            }
            if (!pending_links.empty()) {
                auto link = pending_links.front();
                if (std::get<2>(link)) {
                    if (!make_link_impl(std::get<0>(link), std::get<1>(link))) {
                        lg::Error() << "Unable to queue new link request";
                        // What do? Loop around and pretend nothing happened
                        pw_core_sync(core, PW_ID_CORE, 0);
                    } else {
                        lg::Info() << "Creating link between ports, " << std::get<0>(link) << " and " << std::get<1>(link);
                    }
                } else {
                    auto linkit = link_map.find(std::make_pair(std::get<0>(link), std::get<1>(link)));
                    if (linkit != link_map.end()) {
                        lg::Info() << "Removing link object " << linkit->second << " linking ports " << linkit->first.first << " and " << linkit->first.second;
                        pw_registry_destroy(registry, linkit->second);
                    } else {
                        lg::Warn() << "Unable to find link between ports, " << linkit->first.first << " and " << linkit->first.second << ", cannot remove";
                    }
                    pw_core_sync(core, PW_ID_CORE, 0);
                }
                pending_links.pop_front();
                busy = true;

            } else if (quit){
                lg::Debug() << "Quitting from loop";
                pw_main_loop_quit(loop);
                busy = false;
            } else {
                lg::Debug() << "No new links pending";
                busy = false;
            }
        }

        void run() {

lg::Debug() << "Start loop: PW Wrapper: " << this;

            while (!quit) {
                pw_main_loop_run(loop);
                lg::Info() << "Main loop interrupted " << (quit ? "due to quit":"but quit not set!");
            }

            spa_hook_remove(&core_listener);
            pw_proxy_destroy((struct pw_proxy*)registry);
            lg::Debug() << "PW: cleaning up core connection";
            pw_core_disconnect(core);
            lg::Debug() << "PW: cleaning up context";
            pw_context_destroy(context);
            pw_main_loop_destroy(loop);
            pw_deinit();
            lg::Debug() << "PW: clean up done";
        }

        static void registry_event_global(void *data, uint32_t id,
                uint32_t permissions, const char *type, uint32_t version,
                const struct spa_dict *props)
        {
            const struct spa_dict_item *item;
            PwWrapper *myself = reinterpret_cast<PwWrapper*>(data);
            PwWrapper::obj_t obj;
            spa_dict_for_each(item, props)
                obj[item->key] = item->value;
            obj["type"] = type;
            myself->add_object(id, obj);
        }

        static void registry_event_global_remove(void *data, uint32_t id) {
            PwWrapper *myself = reinterpret_cast<PwWrapper*>(data);
            myself->remove_object(id);
        }

        static void link_proxy_error(void *data, int seq, int res, const char *message) {
            lg::Warn() << "link proxy error : '" << message << "', seq: " << seq << " res: " << res;
        }

        static void link_proxy_destroyed(void *data) {
            lg::Debug() << "link proxy destroyed " << data;
        }

        static void link_proxy_done(void *data, int seq) { 
            lg::Debug() << "link proxy done " << data << " seq " << seq;
        }

        void cleanup_and_process_pending() {
            std::scoped_lock l(m);
            if (link_proxy) {
                spa_hook_remove(&link_proxy_listener);
                pw_proxy_destroy(link_proxy);
                link_proxy = 0;
                if (busy) {
                    lg::Debug() << "removed link proxy";
                } else {
                    // Something bad happened
                    lg::Warn() << "link proxy non-null while not busy!";
                }
            }
            process_pending();
        }

        static void on_core_done(void *data, uint32_t id, int seq) {
            PwWrapper *myself = reinterpret_cast<PwWrapper*>(data);
            lg::Debug() << "core done " << data << " seq " << seq << " link_proxy  (" << myself->link_proxy << ")" ;
            myself->cleanup_and_process_pending();
        }

        struct spa_hook link_proxy_listener = {};

        struct pw_proxy_events const link_proxy_events = {
            PW_VERSION_PROXY_EVENTS,
            .destroy = link_proxy_destroyed,
            .done = link_proxy_done,
            .error = &PwWrapper::link_proxy_error,
        };

        struct pw_core_events const core_events = {
            PW_VERSION_CORE_EVENTS,
            .done = &PwWrapper::on_core_done,
        };

        struct pw_registry_events const registry_events = {
            PW_VERSION_REGISTRY_EVENTS,
            .global = &PwWrapper::registry_event_global,
            .global_remove = &PwWrapper::registry_event_global_remove,
        };

        struct pw_proxy *link_proxy = 0;
        struct pw_main_loop *loop = 0;
        struct pw_context *context = 0;
        struct pw_core *core = 0;
        struct pw_registry *registry = 0;
        struct spa_hook registry_listener = {};
        struct spa_hook core_listener = {};
        std::thread thread;
        bool quit = false;
        std::mutex m;
        bool busy = false;
        std::map<uint32_t, obj_t> objects;
        port_map_t src_port_map;
        port_map_t dst_port_map;
        link_map_t link_map;
        link_pending_list_t pending_links;
        link_name_set_t desired_links;
};


///////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace Http = Pistache::Http;
namespace json = rapidjson;

class Api {
    using Route = Pistache::Rest::Route;
    using Request = Pistache::Rest::Request;
  public:
    explicit Api(PwWrapper & pwwrap, uint16_t port, std::string host="127.0.0.1")
        : httpEndpoint(Pistache::Address(host, Pistache::Port(port)))
        , pw(pwwrap)
    {
        auto opts = Http::Endpoint::options()
                .threads(1);

        httpEndpoint.init(opts);
        set_routes();
        httpEndpoint.setHandler(router.handler());
    }

    void serve() {
        httpEndpoint.serve();
    }
    void stop() {
        httpEndpoint.shutdown();
    }
    void clear_and_stop() {
        pw.set_desired_links(PwWrapper::link_name_list_t());
        httpEndpoint.shutdown();
    }

  private:
    void set_routes() {
        using namespace Pistache::Rest;

        Routes::Get(router, "/ports", Routes::bind(&Api::ports, this));
        Routes::Get(router, "/links/active", Routes::bind(&Api::active_links_get, this));
        Routes::Get(router, "/links/desired", Routes::bind(&Api::desired_links_get, this));
        Routes::Put(router, "/links/desired", Routes::bind(&Api::desired_links_set, this));
        Routes::Patch(router, "/links/desired", Routes::bind(&Api::desired_links_add, this));
        Routes::Post(router, "/links/remove", Routes::bind(&Api::desired_links_remove, this));
    }

    static std::string jsonify(json::Document & doc) {
        std::ostringstream oss;
        json::OStreamWrapper oswrap(oss);
        json::Writer<json::OStreamWrapper> writer(oswrap);
        doc.Accept(writer);
        return oss.str();
    }

    Route::Result ports(const Request& request, Http::ResponseWriter response) {
        auto port_lists = pw.get_port_names();
        json::Document d;
        d.SetArray();

        json::Value srclist(json::kArrayType);
        for(auto & n: port_lists.first)
            srclist.PushBack(json::Value(n.c_str(), n.length(), d.GetAllocator()), d.GetAllocator());        
        d.PushBack(srclist, d.GetAllocator());

        json::Value dstlist(json::kArrayType);
        for(auto & n: port_lists.second)
            dstlist.PushBack(json::Value(n.c_str(), n.length(), d.GetAllocator()), d.GetAllocator());        
        d.PushBack(dstlist, d.GetAllocator());

        response.send(Http::Code::Ok, jsonify(d), MIME(Application, Json));
        return Route::Result::Ok;
    }

    Route::Result active_links_get(const Request& request, Http::ResponseWriter response) {
        auto links = pw.get_active_links();
        json::Document d;
        d.SetArray();

        for(auto & p: links) {
            json::Value link(json::kArrayType);
            link.PushBack(json::Value(p.first.c_str(), p.first.length(), d.GetAllocator()), d.GetAllocator());        
            link.PushBack(json::Value(p.second.c_str(), p.second.length(), d.GetAllocator()), d.GetAllocator());
            d.PushBack(link, d.GetAllocator());
        }

        response.send(Http::Code::Ok, jsonify(d), MIME(Application, Json));
        return Route::Result::Ok;
    }
    Route::Result desired_links_get(const Request& request, Http::ResponseWriter response) {
        auto links = pw.get_desired_links();
        json::Document d;
        d.SetArray();

        for(auto & p: links) {
            json::Value link(json::kArrayType);
            link.PushBack(json::Value(p.first.c_str(), p.first.length(), d.GetAllocator()), d.GetAllocator());        
            link.PushBack(json::Value(p.second.c_str(), p.second.length(), d.GetAllocator()), d.GetAllocator());
            d.PushBack(link, d.GetAllocator());
        }

        response.send(Http::Code::Ok, jsonify(d), MIME(Application, Json));
        return Route::Result::Ok;
    }

    Route::Result desired_links_modify(const Request& request, Http::ResponseWriter & response, void (PwWrapper::*action)(PwWrapper::link_name_list_t const &)) {
        json::Document d;
        bool result = false;
        d.Parse(request.body().c_str());
        lg::Debug() << "Got doc: " << jsonify(d);
        if (d.IsArray()) {
            PwWrapper::link_name_list_t list_in;
            for (auto l = d.Begin() ; l != d.End() ; ++l) {
                if (!l->IsArray() || l->Size() != 2) {
                    lg::Debug() << "Unexpected entry";
                    break;
                }
                list_in.push_back(std::make_pair((*l)[0].GetString(), (*l)[1].GetString()));
            }
            (pw.*action)(list_in);
            result = true;
        }
        if (result)
            response.send(Http::Code::Ok);
        else
            response.send(Http::Code::Not_Acceptable);

        return Route::Result::Ok;
    }

    Route::Result desired_links_set(const Request& request, Http::ResponseWriter response) {
        return desired_links_modify(request, response, &PwWrapper::set_desired_links);
    }
    Route::Result desired_links_add(const Request& request, Http::ResponseWriter response) {
        return desired_links_modify(request, response, &PwWrapper::add_desired_links);
    }
    Route::Result desired_links_remove(const Request& request, Http::ResponseWriter response) {
        return desired_links_modify(request, response, &PwWrapper::remove_desired_links);
    }

    Http::Endpoint httpEndpoint;
    Pistache::Rest::Router router;
    PwWrapper & pw;
};


int main(int argc, char *argv[])
{
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGHUP, handle_sig);
    PwWrapper pww;
    usleep(100000);
    pww.show_ports();
    Api api(pww, 9080);
    quit = std::bind(&Api::clear_and_stop, &api);
    api.serve();
    return 0;
}
