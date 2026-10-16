// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <array>
#include <cstdio> /* fprintf () */
#include <cstdlib> /* atoi () */
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <signal.h>

#include <fmt/format.h>

#include <libtransmission/transmission.h>

#include <libtransmission/env.h>
#include <libtransmission/error.h>
#include <libtransmission/file.h>
#include <libtransmission/tr-getopt.h>
#include <libtransmission/utils.h> // _()
#include <libtransmission/values.h>
#include <libtransmission/variant.h>
#include <libtransmission/version.h>
#include <libtransmission/web-utils.h>
#include <libtransmission/web.h> // tr_sessionFetch()

using namespace std::literals;
using namespace tr::Values;

#define SPEED_K_STR "kB/s"

namespace
{
auto constexpr LineWidth = int{ 80 };

char constexpr MyConfigName[] = "transmission";
char constexpr MyReadableName[] = "transmission-cli";
char constexpr Usage
    [] = "A fast and easy BitTorrent client\n"
         "\n"
         "Usage: transmission-cli [options] <file|url|magnet>";

bool showVersion = false;
bool verify = false;
sig_atomic_t gotsig = false;
sig_atomic_t manualUpdate = false;

char const* torrentPath = nullptr;

using Arg = tr_option::Arg;
static_assert(TrDefaultPeerPort == 51413, "update 'port' desc");
static_assert(TrDefaultPeerSocketTos == "le", "update 'tos' desc");
auto constexpr Options = std::array<tr_option, 29>{ {
    { 'b', "blocklist", "Enable peer blocklists", "b", Arg::None, nullptr },
    { 'B', "no-blocklist", "Disable peer blocklists", "B", Arg::None, nullptr },
    { 'd', "downlimit", "Set max download speed in " SPEED_K_STR, "d", Arg::Required, "<speed>" },
    { 'D', "no-downlimit", "Don't limit the download speed", "D", Arg::None, nullptr },
    { 910, "encryption-required", "Encrypt all peer connections", "er", Arg::None, nullptr },
    { 911, "encryption-preferred", "Prefer encrypted peer connections", "ep", Arg::None, nullptr },
    { 912, "encryption-tolerated", "Prefer unencrypted peer connections", "et", Arg::None, nullptr },
    { 'f', "finish", "Run a script when the torrent finishes", "f", Arg::Required, "<script>" },
    { 'g', "config-dir", "Where to find configuration files", "g", Arg::Required, "<path>" },
    { 'm', "portmap", "Enable portmapping via NAT-PMP or UPnP", "m", Arg::None, nullptr },
    { 'M', "no-portmap", "Disable portmapping", "M", Arg::None, nullptr },
    { 'p', "port", "Port for incoming peers (Default: 51413)", "p", Arg::Required, "<port>" },
    { 't',
      "tos",
      "Peer socket DSCP / ToS. Number or DSCP string, e.g. 'af11' or 'cs0' (Default: 'le')",
      "t",
      Arg::Required,
      "<dscp-or-tos>" },
    { 'u', "uplimit", "Set max upload speed in " SPEED_K_STR, "u", Arg::Required, "<speed>" },
    { 'U', "no-uplimit", "Don't limit the upload speed", "U", Arg::None, nullptr },
    { 'v', "verify", "Verify the specified torrent", "v", Arg::None, nullptr },
    { 'V', "version", "Show version number and exit", "V", Arg::None, nullptr },
    { 'w', "download-dir", "Where to save downloaded data", "w", Arg::Required, "<path>" },
    { 500, "sequential-download", "Download pieces sequentially", "seq", Arg::None, nullptr },
    { 'S', "dont-seed", "Don't seed longer than absolutely necessary", "S", Arg::None, nullptr },
    { 'E', "simple-peer", "Disable uTP, PEX and DHT", "E", Arg::None, nullptr },
    { 'i', "bind-address-ipv4", "Where to listen for peer connections", "i", Arg::Required, "<ipv4 addr>" },
    { 'I', "bind-address-ipv6", "Where to listen for peer connections", "I", Arg::Required, "<ipv6 addr>" },
    { 'r', "rpc-bind-address", "Where to listen for RPC connections", "r", Arg::Required, "<ip addr>" },
    { 's', "stalled-minutes", "Minutes with no data before failing", "s", Arg::Required, "<minutes>" },
    { 600, "bind-interface", "Bind to specific interface", "inf", Arg::Required, "<interface>" },
    { 601, "ban-choking-peers", "Drop and temp-ban peers that choke us", "bch", Arg::None, nullptr },
    { 602, "no-ipv6", "Don't use IPv6 at all", nullptr, Arg::None, nullptr },

    { 0, nullptr, nullptr, nullptr, Arg::None, nullptr },
} };
static_assert(Options[std::size(Options) - 2].val != 0);
} // namespace

namespace
{
void sigHandler(int signal);

[[nodiscard]] std::string tr_strlratio(double ratio)
{
    if (static_cast<int>(ratio) == TR_RATIO_NA)
    {
        return _("None");
    }

    if (static_cast<int>(ratio) == TR_RATIO_INF)
    {
        return _("Inf");
    }

    if (ratio < 10.0)
    {
        return fmt::format("{:.2f}", ratio);
    }

    if (ratio < 100.0)
    {
        return fmt::format("{:.1f}", ratio);
    }

    return fmt::format("{:.0f}", ratio);
}

bool waitingOnWeb;

void onTorrentFileDownloaded(tr_web::FetchResponse const& response)
{
    auto* ctor = static_cast<tr_ctor*>(response.user_data);
    tr_ctorSetMetainfo(ctor, std::data(response.body), std::size(response.body), nullptr);
    waitingOnWeb = false;
}

[[nodiscard]] std::string getStatusStr(tr_stat const& st)
{
    if (st.activity == TR_STATUS_CHECK_WAIT)
    {
        return "Waiting to verify local files";
    }

    if (st.activity == TR_STATUS_CHECK)
    {
        return fmt::format(
            "Verifying local files ({:.2f}%, {:.2f}% valid)",
            tr_truncd(100 * st.recheck_progress, 2),
            tr_truncd(100 * st.percent_done, 2));
    }

    if (st.activity == TR_STATUS_DOWNLOAD)
    {
        auto dl_buf = std::array<char, 64>{};
        auto ul_buf = std::array<char, 64>{};
        auto const dl_speed = st.piece_download_speed.to_string(std::data(dl_buf), std::size(dl_buf));
        auto const ul_speed = st.piece_upload_speed.to_string(std::data(ul_buf), std::size(ul_buf));
        auto const ratio_str = tr_strlratio(st.upload_ratio);
        return fmt::format(
            "Progress: {:.1f}%, dl from {:d} of {:d} peers ({:s}), ul to {:d} "
            "({:s}) [{:s}]",
            tr_truncd(100 * st.percent_done, 1),
            st.peers_sending_to_us,
            st.peers_connected,
            dl_speed,
            st.peers_getting_from_us,
            ul_speed,
            ratio_str);
    }

    if (st.activity == TR_STATUS_SEED)
    {
        auto ul_buf = std::array<char, 64>{};
        auto const ul_speed = st.piece_upload_speed.to_string(std::data(ul_buf), std::size(ul_buf));
        auto const ratio_str = tr_strlratio(st.upload_ratio);
        return fmt::format(
            "Seeding, uploading to {:d} of {:d} peer(s), {:s} [{:s}]",
            st.peers_getting_from_us,
            st.peers_connected,
            ul_speed,
            ratio_str);
    }

    return {};
}

[[nodiscard]] std::string getConfigDir(int argc, char const** argv)
{
    int c;
    char const* my_optarg;
    int const ind = tr_optind;

    while ((c = tr_getopt(Usage, argc, argv, std::data(Options), &my_optarg)) != TR_OPT_DONE)
    {
        if (c == 'g')
        {
            tr_optind = ind;
            return my_optarg;
        }
    }

    tr_optind = ind;

    return tr_getDefaultConfigDir(MyConfigName);
}

// ---

int parseCommandLine(tr_variant::Map& map, int argc, char const** argv)
{
    int c;
    char const* my_optarg;

    while ((c = tr_getopt(Usage, argc, argv, std::data(Options), &my_optarg)) != TR_OPT_DONE)
    {
        switch (c)
        {
        case 'b':
            map[TR_KEY_blocklist_enabled] = true;
            break;

        case 'B':
            map[TR_KEY_blocklist_enabled] = false;
            break;

        case 'd':
            map[TR_KEY_speed_limit_down] = int64_t(atoi(my_optarg));
            map[TR_KEY_speed_limit_down_enabled] = true;
            break;

        case 'D':
            map[TR_KEY_speed_limit_down_enabled] = false;
            break;

        case 'f':
            map[TR_KEY_script_torrent_done_filename] = my_optarg;
            map[TR_KEY_script_torrent_done_enabled] = true;
            break;

        case 'g': /* handled above */
            break;

        case 'm':
            map[TR_KEY_port_forwarding_enabled] = true;
            break;

        case 'M':
            map[TR_KEY_port_forwarding_enabled] = false;
            break;

        case 'p':
            map[TR_KEY_peer_port] = int64_t(atoi(my_optarg));
            break;

        case 't':
            map[TR_KEY_peer_socket_diffserv] = my_optarg;
            break;

        case 'u':
            map[TR_KEY_speed_limit_up] = int64_t(atoi(my_optarg));
            map[TR_KEY_speed_limit_up_enabled] = true;
            break;

        case 'U':
            map[TR_KEY_speed_limit_up_enabled] = false;
            break;

        case 'v':
            verify = true;
            break;

        case 'V':
            showVersion = true;
            break;

        case 'S':
            map[TR_KEY_seed_ratio_limited] = true;
            map[TR_KEY_seed_ratio_limit] = 0.0;
            map[TR_KEY_idle_seeding_limit_enabled] = true;
            map[TR_KEY_idle_seeding_limit] = int64_t{ 1 };
            break;

        case 'E':
            // disable dht, pex and utp
            map[TR_KEY_dht_enabled] = false;
            map[TR_KEY_pex_enabled] = false;
            map[TR_KEY_utp_enabled] = false;
            break;

        case 'i':
            map[TR_KEY_bind_address_ipv4] = my_optarg;
            break;

        case 'I':
            map[TR_KEY_bind_address_ipv6] = my_optarg;
            break;

        case 'r':
            map[TR_KEY_rpc_bind_address] = my_optarg;
            break;

        case 600:
            map[TR_KEY_bind_interface] = my_optarg;
            break;

        case 601:
            map[TR_KEY_ban_choking_peers] = true;
            break;

        case 602:
            map[TR_KEY_ipv6_enabled] = false;
            break;

        case 'w':
            map[TR_KEY_download_dir] = my_optarg;
            break;

        case 910:
            map[TR_KEY_encryption] = int64_t(TR_ENCRYPTION_REQUIRED);
            break;

        case 911:
            map[TR_KEY_encryption] = int64_t(TR_ENCRYPTION_PREFERRED);
            break;

        case 912:
            map[TR_KEY_encryption] = int64_t(TR_CLEAR_PREFERRED);
            break;

        case 500:
            map[TR_KEY_sequential_download] = true;
            break;

        case 's':
            map[TR_KEY_queue_stalled_enabled] = true;
            map[TR_KEY_queue_stalled_minutes] = int64_t(atoi(my_optarg));
            break;

        case TR_OPT_UNK:
            if (torrentPath == nullptr)
            {
                torrentPath = my_optarg;
            }

            break;

        default:
            return 1;
        }
    }

    return 0;
}

void sigHandler(int signal)
{
    switch (signal)
    {
    case SIGINT:
        gotsig = true;
        break;

#ifndef _WIN32

    case SIGHUP:
        manualUpdate = true;
        break;

#endif

    default:
        break;
    }
}

[[nodiscard]] constexpr std::string_view getErrorMessagePrefix(auto const err)
{
    switch (err)
    {
    case tr_stat::Error::TrackerWarning:
        return "Tracker gave a warning:"sv;
    case tr_stat::Error::TrackerError:
        return "Tracker gave an error:"sv;
    case tr_stat::Error::LocalError:
        return "Error:"sv;
    case tr_stat::Error::Ok:
        return ""sv;
    }

    return ""sv;
}
} // namespace

int tr_main(int argc, char* argv[])
{
    tr_lib_init();

    tr_locale_set_global("");

    printf("%s %s\n", MyReadableName, LONG_VERSION_STRING);

    /* user needs to pass in at least one argument */
    if (argc < 2)
    {
        tr_getopt_usage(MyReadableName, Usage, std::data(Options));
        return EXIT_FAILURE;
    }

    /* load the defaults from config file + libtransmission defaults */
    auto const config_dir = getConfigDir(argc, (char const**)argv);
    auto settings = tr_sessionLoadSettings(config_dir);

    /* the command line overrides defaults */
    if (parseCommandLine(*settings.get_if<tr_variant::Map>(), argc, (char const**)argv) != 0)
    {
        return EXIT_FAILURE;
    }

    if (showVersion)
    {
        return EXIT_SUCCESS;
    }

    /* Check the options for validity */
    if (torrentPath == nullptr)
    {
        fprintf(stderr, "No torrent specified!\n");
        return EXIT_FAILURE;
    }

    auto* const h = tr_sessionInit(config_dir, false, settings);
    auto* const ctor = tr_ctorNew(h);

    tr_ctorSetPaused(ctor, TR_FORCE, false);

    if (tr_sys_path_exists(torrentPath) ? tr_ctorSetMetainfoFromFile(ctor, torrentPath) :
                                          tr_ctorSetMetainfoFromMagnetLink(ctor, torrentPath))
    {
        // all good
    }
    else if (tr_urlIsValid(torrentPath))
    {
        // fetch it
        tr_sessionFetch(h, { torrentPath, onTorrentFileDownloaded, ctor });
        waitingOnWeb = true;
        while (waitingOnWeb)
        {
            std::this_thread::sleep_for(1s);
        }
    }
    else
    {
        fprintf(stderr, "ERROR: Unrecognized torrent \"%s\".\n", torrentPath);
        fprintf(stderr, " * If you're trying to create a torrent, use transmission-create.\n");
        fprintf(
            stderr,
            " * If you're trying to see a torrent's info, use "
            "transmission-show.\n");
        tr_sessionClose(h);
        return EXIT_FAILURE;
    }

    tr_torrent* tor = tr_torrentNew(ctor, nullptr);
    tr_ctorFree(ctor);
    if (tor == nullptr)
    {
        fprintf(stderr, "Failed opening torrent file `%s'\n", torrentPath);
        tr_sessionClose(h);
        return EXIT_FAILURE;
    }

    signal(SIGINT, sigHandler);
#ifndef _WIN32
    signal(SIGHUP, sigHandler);
#endif
    tr_torrentStart(tor);

    if (verify)
    {
        verify = false;
        tr_torrentVerify(tor);
    }

    for (;;)
    {
        std::this_thread::sleep_for(200ms);

        if (gotsig)
        {
            gotsig = false;
            printf("\nStopping torrent...\n");
            tr_torrentStop(tor);
        }

        if (manualUpdate)
        {
            manualUpdate = false;

            if (!tr_torrentCanManualUpdate(tor))
            {
                fprintf(stderr, "\nReceived SIGHUP, but can't send a manual update now\n");
            }
            else
            {
                fprintf(stderr, "\nReceived SIGHUP: manual update scheduled\n");
                tr_torrentManualUpdate(tor);
            }
        }

        auto const st = tr_torrentStat(tor);

        if (st.activity == TR_STATUS_STOPPED)
        {
            break;
        }

        auto const status_str = getStatusStr(st);
        printf("\r%-*s", LineWidth, status_str.c_str());
        fflush(stdout);

        auto const* const settings_map = settings.get_if<tr_variant::Map>();
        auto const ratio_limit_enabled = settings_map != nullptr &&
            settings_map->value_if<bool>(TR_KEY_seed_ratio_limited).value_or(false);
        auto const ratio_limit = settings_map != nullptr ? settings_map->value_if<double>(TR_KEY_seed_ratio_limit) :
                                                           std::optional<double>{};
        if (ratio_limit_enabled && ratio_limit && *ratio_limit <= 0.0 && st.activity == TR_STATUS_SEED)
        {
            break;
        }

        if (st.is_stalled)
        {
            fprintf(stderr, "Torrent `%s' has stalled\n", torrentPath);
            tr_sessionClose(h);
            return EXIT_FAILURE;
        }

        if (st.error != tr_stat::Error::Ok)
        {
            fmt::print(stderr, "\n{:s}: {:s}\n", getErrorMessagePrefix(st.error), st.error_string);
        }
    }

    tr_sessionSaveSettings(h, config_dir, settings);

    printf("\n");
    tr_sessionClose(h);
    return EXIT_SUCCESS;
}
