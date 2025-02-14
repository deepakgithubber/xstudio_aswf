// SPDX-License-Identifier: Apache-2.0
#include <caf/policy/select_all.hpp>
#include <chrono>
#include <regex>
#include <tuple>

#include "xstudio/atoms.hpp"
#include "xstudio/broadcast/broadcast_actor.hpp"
#include "xstudio/global_store/global_store.hpp"
#include "xstudio/json_store/json_store_actor.hpp"
#include "xstudio/media/media_actor.hpp"
#include "xstudio/playhead/sub_playhead.hpp"
#include "xstudio/utility/helpers.hpp"
#include "xstudio/utility/logging.hpp"
#include "xstudio/utility/uuid.hpp"
#include "xstudio/thumbnail/thumbnail.hpp"

using namespace std::chrono_literals;
using namespace caf;

using namespace xstudio;
using namespace xstudio::utility;
using namespace xstudio::json_store;
using namespace xstudio::media;
using namespace xstudio::media_reader;
using namespace xstudio::media_metadata;
using namespace xstudio::global_store;

#define ERR_HANDLER_FUNC                                                                       \
    [=](error &err) mutable { spdlog::warn("{} {}", __PRETTY_FUNCTION__, to_string(err)); }

MediaSourceActor::MediaSourceActor(caf::actor_config &cfg, const JsonStore &jsn)
    : caf::event_based_actor(cfg), base_(static_cast<JsonStore>(jsn["base"])), parent_() {
    if (not jsn.count("store") or jsn["store"].is_null()) {
        json_store_ = spawn<json_store::JsonStoreActor>(
            utility::Uuid::generate(), utility::JsonStore(), std::chrono::milliseconds(50));
    } else {
        json_store_ = spawn<json_store::JsonStoreActor>(
            utility::Uuid::generate(),
            static_cast<JsonStore>(jsn["store"]),
            std::chrono::milliseconds(50));
    }
    link_to(json_store_);

    for (const auto &[key, value] : jsn["actors"].items()) {
        if (value["base"]["container"]["type"] == "MediaStream") {
            try {
                media_streams_[Uuid(key)] =
                    system().spawn<media::MediaStreamActor>(static_cast<JsonStore>(value));
                link_to(media_streams_[Uuid(key)]);
                join_event_group(this, media_streams_[Uuid(key)]);
            } catch (const std::exception &e) {
                spdlog::warn("{} {}", __PRETTY_FUNCTION__, e.what());
            }
        }
    }

    init();
}

MediaSourceActor::MediaSourceActor(
    caf::actor_config &cfg,
    const std::string &name,
    const caf::uri &_uri,
    const FrameList &frame_list,
    const utility::FrameRate &rate,
    const utility::Uuid &uuid)
    : caf::event_based_actor(cfg), base_(name, _uri, frame_list), parent_() {
    if (not uuid.is_null())
        base_.set_uuid(uuid);

    json_store_ = spawn<json_store::JsonStoreActor>(
        utility::Uuid::generate(), utility::JsonStore(), std::chrono::milliseconds(50));
    link_to(json_store_);

    // need this on creation or other functions randomly fail, as streams aren't configured..
    anon_send(actor_cast<actor>(this), acquire_media_detail_atom_v, rate);
    // acquire_detail(rate);

    init();
}

MediaSourceActor::MediaSourceActor(
    caf::actor_config &cfg,
    const std::string &name,
    const caf::uri &_uri,
    const utility::FrameRate &rate,
    const utility::Uuid &uuid)
    : caf::event_based_actor(cfg), base_(name, _uri), parent_() {
    if (not uuid.is_null())
        base_.set_uuid(uuid);
    json_store_ = spawn<json_store::JsonStoreActor>(
        utility::Uuid::generate(), utility::JsonStore(), std::chrono::milliseconds(50));
    link_to(json_store_);

    // need this on creation or other functions randomly fail, as streams aren't configured..
    anon_send(actor_cast<actor>(this), acquire_media_detail_atom_v, rate);
    // acquire_detail(rate);

    init();
}

MediaSourceActor::MediaSourceActor(
    caf::actor_config &cfg,
    const std::string &name,
    const std::string &reader,
    const utility::MediaReference &media_reference,
    const utility::Uuid &uuid)
    : caf::event_based_actor(cfg), base_(name, media_reference), parent_() {
    if (not uuid.is_null())
        base_.set_uuid(uuid);
    base_.set_reader(reader);
    json_store_ = spawn<json_store::JsonStoreActor>(
        utility::Uuid::generate(), utility::JsonStore(), std::chrono::milliseconds(50));
    link_to(json_store_);

    base_.media_reference().set_timecode_from_frames();

    init();
}

#include <chrono>

void MediaSourceActor::acquire_detail(
    const utility::FrameRate &rate, caf::typed_response_promise<bool> rp) {

    // is this a good idea ? We can never update the details.
    if (media_streams_.size()) {
        rp.deliver(true);
        return;
    } else if (not base_.online()) {
        rp.deliver(false);
        return;
    }

    // clear current settings, probably irrelvant because above.
    for (auto &i : media_streams_) {
        unlink_from(i.second);
        send_exit(i.second, caf::exit_reason::user_shutdown);
    }
    media_streams_.clear();
    base_.clear();

    try {
        auto gmra = system().registry().template get<caf::actor>(media_reader_registry);
        if (gmra) {
            int frame;
            auto _uri = base_.media_reference().uri(0, frame);
            if (not _uri)
                throw std::runtime_error("Invalid frame index");

            request(
                gmra, infinite, get_media_detail_atom_v, *_uri, actor_cast<actor_addr>(this))
                .then(
                    [=](const MediaDetail &md) mutable {
                        if (not base_.media_reference().timecode().total_frames())
                            base_.media_reference().set_timecode(md.timecode_);
                        base_.set_reader(md.reader_);

                        for (auto i : md.streams_) {
                            // HACK!!!
                            if (i.media_type_ == MT_IMAGE) {
                                // we don't know duration, either movie or single frame
                                if (not base_.media_reference().duration().duration().count()) {
                                    // movie..
                                    if (i.duration_.duration().count()) {
                                        base_.media_reference().set_duration(i.duration_);
                                        base_.media_reference().set_frame_list(
                                            FrameList(0, i.duration_.frames() - 1));
                                    } else {
                                        if (i.duration_.rate().count()) {
                                            base_.media_reference().set_duration(
                                                FrameRateDuration(1, i.duration_.rate()));
                                            i.duration_ =
                                                FrameRateDuration(1, i.duration_.rate());
                                        } else {
                                            base_.media_reference().set_duration(
                                                FrameRateDuration(1, rate));
                                            i.duration_ = FrameRateDuration(1, rate);
                                        }
                                        base_.media_reference().set_frame_list(FrameList(0, 0));
                                    }
                                }
                                // we know duration but not rate
                                else if (i.duration_.rate().count()) {
                                    // we know duration, so override rate.
                                    // effects count..
                                    int frames  = base_.media_reference().duration().frames();
                                    i.duration_ = FrameRateDuration(frames, i.duration_.rate());

                                    base_.media_reference().set_duration(
                                        FrameRateDuration(frames, i.duration_.rate()));
                                } else {
                                    if (not base_.media_reference().container()) {
                                        int frames =
                                            base_.media_reference().duration().frames();
                                        i.duration_ = FrameRateDuration(frames, rate);
                                    } else {
                                        i.duration_.set_rate(rate);
                                    }

                                    base_.media_reference().set_rate(rate);
                                }
                            }

                            auto uuid   = utility::Uuid::generate();
                            auto stream = spawn<MediaStreamActor>(
                                i.name_, i.duration_, i.media_type_, i.key_format_, uuid);
                            link_to(stream);
                            join_event_group(this, stream);
                            media_streams_[uuid] = stream;
                            base_.add_media_stream(i.media_type_, uuid);
                            send(
                                event_group_,
                                utility::event_atom_v,
                                add_media_stream_atom_v,
                                UuidActor(uuid, stream));

                            spdlog::debug(
                                "Media {} fps, {} frames {} timecode.",
                                base_.media_reference().rate().to_fps(),
                                base_.media_reference().frame_count(),
                                to_string(base_.media_reference().timecode()));
                        }
                        request(
                            actor_cast<caf::actor>(this),
                            infinite,
                            media_metadata::get_metadata_atom_v)
                            .then(
                                [=](const bool) mutable {
                                    anon_send(this, media_hook::get_media_hook_atom_v);
                                },
                                [=](const caf::error &err) mutable {
                                    spdlog::debug(
                                        "{} {} {}",
                                        __PRETTY_FUNCTION__,
                                        to_string(err),
                                        to_string(base_.media_reference().uri()));
                                    anon_send(this, media_hook::get_media_hook_atom_v);
                                });
                        if (not base_.media_reference().container() and
                            (!base_.media_reference().timecode().total_frames() ||
                             base_.media_reference().frame_list().start())) {
                            // If we have image sequence (like EXRs, say) where the frame number
                            // from the filename is 1001, then we use the frame number to set
                            // the timecode on this source. This means timecode == frame number
                            // so we are OVERRIDING the timecode embedded in EXR header data
                            // with a timecode from frame number. This is because frame number
                            // is paramount in aligning media in a timeline, the embedded
                            // timecode is rarely used for this purpose. Also, if the timecode
                            // is unknown (or is 00:00:00:00) then we default to using frame
                            // number to set the timecode.
                            base_.media_reference().set_timecode_from_frames();
                        }

                        base_.send_changed(event_group_, this);
                        send(event_group_, utility::event_atom_v, change_atom_v);

                        rp.deliver(true);
                    },
                    [=](const error &err) mutable {
                        // set media status..
                        // set duration to one frame. Or things get upset.
                        // base_.media_reference().set_duration(
                        //     FrameRateDuration(1, base_.media_reference().duration().rate()));
                        spdlog::debug("{} {}", __PRETTY_FUNCTION__, to_string(err));
                        base_.send_changed(event_group_, this);
                        send(event_group_, utility::event_atom_v, change_atom_v);
                        base_.set_error_detail(to_string(err));
                        rp.deliver(false);
                    });
        } else {
            rp.deliver(false);
        }
    } catch (const std::exception &err) {
        base_.set_error_detail(err.what());
        rp.deliver(false);
    }
}

void MediaSourceActor::init() {
    print_on_create(this, base_);
    print_on_exit(this, base_);

    {
        auto scanner = system().registry().template get<caf::actor>(scanner_registry);
        if (scanner)
            anon_send(scanner, media_status_atom_v, base_.media_reference(), this);
    }

    event_group_ = spawn<broadcast::BroadcastActor>(this);
    link_to(event_group_);

// set an empty dict for colour_pipeline, as we request this at various
// times and need a placeholder or we get warnings if it's not there
#pragma message "This should not be here, this is plugin specific."
    request(json_store_, infinite, json_store::get_json_atom_v, "/colour_pipeline")
        .then(
            [=](const JsonStore &) {},
            [=](const error &) {
                // we'll get this error if there is no dict already
                anon_send(
                    json_store_,
                    json_store::set_json_atom_v,
                    utility::JsonStore(),
                    "/colour_pipeline");
            });

    auto thumbnail_manager = system().registry().get<caf::actor>(thumbnail_manager_registry);

    behavior_.assign(
        base_.make_set_name_handler(event_group_, this),
        base_.make_get_name_handler(),
        base_.make_last_changed_getter(),
        base_.make_last_changed_setter(event_group_, this),
        base_.make_last_changed_event_handler(event_group_, this),
        base_.make_get_uuid_handler(),
        base_.make_get_type_handler(),
        make_get_event_group_handler(event_group_),
        base_.make_get_detail_handler(this, event_group_),
        [=](xstudio::broadcast::broadcast_down_atom, const caf::actor_addr &) {},

        [=](acquire_media_detail_atom, const utility::FrameRate &rate) -> result<bool> {
            auto rp = make_response_promise<bool>();
            acquire_detail(rate, rp);
            // why ?
            send(event_group_, utility::event_atom_v, utility::name_atom_v, base_.name());
            return rp;
        },

        [=](media_status_atom) -> MediaStatus { return base_.media_status(); },

        [=](media_status_atom, const MediaStatus status) -> bool {
            if (base_.media_status() != status) {
                base_.set_media_status(status);
                base_.send_changed(event_group_, this);
            }
            return true;
        },

        [=](add_media_stream_atom, caf::actor media_stream) -> result<UuidActor> {
            auto rp = make_response_promise<UuidActor>();
            request(media_stream, infinite, uuid_atom_v)
                .then(
                    [=](const Uuid &uuid) mutable {
                        request(
                            actor_cast<caf::actor>(this),
                            infinite,
                            add_media_stream_atom_v,
                            UuidActor(uuid, media_stream))
                            .then(
                                [=](const UuidActor &ua) mutable { rp.deliver(ua); },
                                [=](const error &err) mutable { rp.deliver(err); });
                    },
                    [=](const error &err) mutable { rp.deliver(err); });
            return rp;
        },

        [=](add_media_stream_atom,
            const utility::UuidActor &media_stream) -> result<UuidActor> {
            auto rp = make_response_promise<UuidActor>();
            request(media_stream.actor(), infinite, get_media_type_atom_v)
                .then(
                    [=](const MediaType &mt) mutable {
                        join_event_group(this, media_stream.actor());
                        link_to(media_stream.actor());
                        media_streams_[media_stream.uuid()] = media_stream.actor();
                        base_.add_media_stream(mt, media_stream.uuid());
                        base_.send_changed(event_group_, this);
                        send(
                            event_group_,
                            utility::event_atom_v,
                            add_media_stream_atom_v,
                            media_stream);
                        rp.deliver(media_stream);
                    },
                    [=](const error &err) mutable { rp.deliver(err); });
            return rp;
        },

        [=](colour_pipeline::get_colour_pipe_params_atom) {
            delegate(json_store_, json_store::get_json_atom_v, "/colour_pipeline");
        },

        [=](colour_pipeline::set_colour_pipe_params_atom, const utility::JsonStore &params) {
            delegate(json_store_, json_store::set_json_atom_v, params, "/colour_pipeline");
            base_.send_changed(event_group_, this);
            send(event_group_, utility::event_atom_v, change_atom_v);
        },

        [=](current_media_stream_atom, const MediaType media_type) -> result<UuidActor> {
            if (media_streams_.count(base_.current(media_type)))
                return UuidActor(
                    base_.current(media_type), media_streams_.at(base_.current(media_type)));
            return result<UuidActor>(make_error(xstudio_error::error, "No streams"));
        },

        [=](current_media_stream_atom, const MediaType media_type, const Uuid &uuid) -> bool {
            auto result = base_.set_current(media_type, uuid);
            if (result)
                base_.send_changed(event_group_, this);

            return result;
        },

        [=](get_edit_list_atom, const Uuid &uuid) -> utility::EditList {
            if (uuid.is_null())
                return utility::EditList({EditListSection(
                    base_.uuid(),
                    base_.media_reference().duration(),
                    base_.media_reference().timecode())});
            return utility::EditList({EditListSection(
                uuid, base_.media_reference().duration(), base_.media_reference().timecode())});
        },

        [=](get_media_details_atom, caf::actor ui_actor) {
            try {
                send_source_details_to_ui(ui_actor);
                send_stream_details_to_ui(ui_actor);
            } catch (const std::exception &err) {
                // I don't trust these won't throw...
                spdlog::warn("{} {}", __PRETTY_FUNCTION__, err.what());
            }
        },

        [=](get_media_pointer_atom atom) {
            delegate(caf::actor_cast<actor>(this), atom, MT_IMAGE);
        },

        [=](get_media_pointer_atom atom,
            const std::vector<std::pair<int, utility::time_point>> &logical_frames) {
            delegate(caf::actor_cast<actor>(this), atom, MT_IMAGE, logical_frames);
        },

        [=](get_media_pointer_atom atom, const int logical_frame) {
            delegate(caf::actor_cast<actor>(this), atom, MT_IMAGE, logical_frame);
        },

        [=](get_media_pointer_atom,
            const MediaType media_type) -> result<std::vector<media::AVFrameID>> {
            auto rp = make_response_promise<std::vector<media::AVFrameID>>();

            if (base_.current(media_type).is_null()) {
                rp.deliver(make_error(xstudio_error::error, "No streams"));
                return rp;
            }

            request(
                media_streams_.at(base_.current(media_type)),
                infinite,
                get_stream_detail_atom_v)
                .then(
                    [=](const StreamDetail &detail) mutable {
                        if (media_type == MT_IMAGE) {
                            request(
                                json_store_,
                                infinite,
                                json_store::get_json_atom_v,
                                "/colour_pipeline")
                                .then(
                                    [=](const JsonStore &meta) mutable {
                                        try {
                                            std::vector<AVFrameID> results;
                                            auto first_frame =
                                                *(base_.media_reference().frame(0));
                                            for (const auto &i :
                                                 base_.media_reference().uris()) {
                                                results.emplace_back(media::AVFrameID(
                                                    i.first,
                                                    i.second,
                                                    first_frame,
                                                    base_.media_reference().rate(),
                                                    detail.name_,
                                                    detail.key_format_,
                                                    base_.reader(),
                                                    caf::actor_cast<caf::actor_addr>(this),
                                                    meta,
                                                    base_.current(MT_IMAGE),
                                                    parent_uuid_,
                                                    media_type));
                                            }

                                            rp.deliver(results);
                                        } catch (const std::exception &e) {
                                            rp.deliver(
                                                make_error(xstudio_error::error, e.what()));
                                        }
                                    },
                                    [=](error &) mutable {
                                        try {
                                            std::vector<AVFrameID> results;
                                            auto first_frame =
                                                *(base_.media_reference().frame(0));
                                            for (const auto &i :
                                                 base_.media_reference().uris()) {
                                                results.emplace_back(media::AVFrameID(
                                                    i.first,
                                                    i.second,
                                                    first_frame,
                                                    base_.media_reference().rate(),
                                                    detail.name_,
                                                    detail.key_format_,
                                                    base_.reader(),
                                                    caf::actor_cast<caf::actor_addr>(this),
                                                    utility::JsonStore(),
                                                    utility::Uuid(),
                                                    parent_uuid_,
                                                    media_type));
                                            }

                                            rp.deliver(results);
                                        } catch (const std::exception &e) {
                                            rp.deliver(
                                                make_error(xstudio_error::error, e.what()));
                                        }
                                    });
                        } else {
                            std::vector<AVFrameID> results;
                            auto first_frame = *(base_.media_reference().frame(0));
                            for (const auto &i : base_.media_reference().uris()) {
                                results.emplace_back(media::AVFrameID(
                                    i.first,
                                    i.second,
                                    first_frame,
                                    base_.media_reference().rate(),
                                    detail.name_,
                                    detail.key_format_,
                                    base_.reader(),
                                    caf::actor_cast<caf::actor_addr>(this),
                                    utility::JsonStore(),
                                    base_.current(media_type),
                                    parent_uuid_,
                                    media_type));
                            }

                            rp.deliver(results);
                        }
                    },
                    [=](error &err) mutable { rp.deliver(std::move(err)); });
            return rp;
        },

        [=](get_media_pointer_atom,
            const MediaType media_type,
            const int logical_frame) -> result<media::AVFrameID> {
            auto rp = make_response_promise<media::AVFrameID>();

            if (base_.current(media_type).is_null()) {
                rp.deliver(make_error(xstudio_error::error, "No streams"));
                return rp;
            }

            request(
                media_streams_.at(base_.current(media_type)),
                infinite,
                get_stream_detail_atom_v)
                .then(
                    [=](const StreamDetail &detail) mutable {
                        try {
                            int frame;
                            auto _uri = base_.media_reference().uri(logical_frame, frame);
                            if (not _uri) {
                                throw std::runtime_error("Invalid frame index");
                            }

                            if (media_type == MT_IMAGE) {
                                // get colours params
                                request(
                                    json_store_,
                                    infinite,
                                    json_store::get_json_atom_v,
                                    "/colour_pipeline")
                                    .then(
                                        [=](const JsonStore &meta) mutable {
                                            rp.deliver(media::AVFrameID(
                                                *_uri,
                                                frame,
                                                *(base_.media_reference().frame(0)),
                                                base_.media_reference().rate(),
                                                detail.name_,
                                                detail.key_format_,
                                                base_.reader(),
                                                caf::actor_cast<caf::actor_addr>(this),
                                                meta,
                                                base_.current(media_type),
                                                parent_uuid_,
                                                media_type));
                                        },
                                        [=](error &) mutable {
                                            rp.deliver(media::AVFrameID(
                                                *_uri,
                                                frame,
                                                *(base_.media_reference().frame(0)),
                                                base_.media_reference().rate(),
                                                detail.name_,
                                                detail.key_format_,
                                                base_.reader(),
                                                caf::actor_cast<caf::actor_addr>(this),
                                                utility::JsonStore(),
                                                utility::Uuid(),
                                                parent_uuid_,
                                                media_type));
                                        });
                            } else {
                                rp.deliver(media::AVFrameID(
                                    *_uri,
                                    frame,
                                    *(base_.media_reference().frame(0)),
                                    base_.media_reference().rate(),
                                    detail.name_,
                                    detail.key_format_,
                                    base_.reader(),
                                    caf::actor_cast<caf::actor_addr>(this),
                                    utility::JsonStore(),
                                    base_.current(media_type),
                                    parent_uuid_,
                                    media_type));
                            }
                        } catch (const std::exception &e) {
                            rp.deliver(make_error(xstudio_error::error, e.what()));
                        }
                    },
                    [=](error &err) mutable { rp.deliver(std::move(err)); });

            return rp;
        },

        [=](get_media_pointers_atom,
            const MediaType media_type,
            const LogicalFrameRanges &ranges) -> caf::result<media::AVFrameIDs> {
            if (base_.empty()) {
                if (base_.error_detail().empty()) {
                    return make_error(xstudio_error::error, "No MediaStreams");
                } else {
                    return make_error(xstudio_error::error, base_.error_detail());
                }
            }

            auto rp = make_response_promise<media::AVFrameIDs>();
            get_media_pointers_for_frames(media_type, ranges, rp);
            return rp;
        },

        [=](media_reader::cancel_thumbnail_request_atom atom, const utility::Uuid job_uuid) {
            anon_send(thumbnail_manager, atom, job_uuid);
        },

        [=](media_reader::get_thumbnail_atom,
            float position,
            const utility::Uuid job_uuid,
            caf::actor requester) {
            int frame = (int)round(float(base_.media_reference().frame_count()) * position);
            frame     = std::max(0, std::min(frame, base_.media_reference().frame_count() - 1));
            request(
                caf::actor_cast<caf::actor>(this),
                infinite,
                get_media_pointer_atom_v,
                MT_IMAGE,
                frame)
                .then(
                    [=](const media::AVFrameID &mp) mutable {
                        request(
                            thumbnail_manager,
                            infinite,
                            media_reader::get_thumbnail_atom_v,
                            mp,
                            job_uuid)
                            .then(
                                [=](const thumbnail::ThumbnailBufferPtr &buf) mutable {
                                    anon_send(
                                        requester, buf, position, job_uuid, std::string());
                                },
                                [=](error &err) mutable {
                                    anon_send(
                                        requester,
                                        thumbnail::ThumbnailBufferPtr(),
                                        0.0f,
                                        job_uuid,
                                        to_string(err));
                                });
                    },
                    [=](error &err) mutable {
                        anon_send(
                            requester,
                            thumbnail::ThumbnailBufferPtr(),
                            0.0f,
                            job_uuid,
                            to_string(err));
                    });
        },

        [=](media_reference_atom) -> MediaReference { return base_.media_reference(); },

        [=](media_reference_atom, const MediaReference &mr) -> bool {
            base_.media_reference() = mr;
            base_.send_changed(event_group_, this);
            send(event_group_, utility::event_atom_v, change_atom_v);
            return true;
        },

        [=](media_reference_atom, const Uuid &uuid) -> std::pair<Uuid, MediaReference> {
            if (uuid.is_null())
                return std::pair<Uuid, MediaReference>(base_.uuid(), base_.media_reference());
            return std::pair<Uuid, MediaReference>(uuid, base_.media_reference());
        },

        [=](get_media_stream_atom, const MediaType media_type) -> std::vector<UuidActor> {
            std::vector<UuidActor> sm;

            for (const auto &i : base_.streams(media_type))
                sm.emplace_back(UuidActor(i, media_streams_.at(i)));

            return sm;
        },

        [=](get_media_stream_atom, const Uuid &uuid) -> result<caf::actor> {
            if (media_streams_.count(uuid))
                return media_streams_.at(uuid);
            return result<caf::actor>(make_error(xstudio_error::error, "Invalid stream uuid"));
        },

        [=](get_media_type_atom, const MediaType media_type) -> bool {
            return base_.has_type(media_type);
        },

        [=](get_stream_detail_atom, const MediaType media_type) -> result<StreamDetail> {
            if (media_streams_.count(base_.current(media_type))) {
                auto rp = make_response_promise<StreamDetail>();
                request(
                    media_streams_.at(base_.current(media_type)),
                    infinite,
                    get_stream_detail_atom_v)
                    .then(
                        [=](const StreamDetail &sd) mutable { rp.deliver(sd); },
                        [=](error &err) mutable { rp.deliver(std::move(err)); });
                return rp;
            }

            return result<StreamDetail>(make_error(xstudio_error::error, "No streams"));
        },

        [=](json_store::get_json_atom atom, const std::string &path) {
            delegate(json_store_, atom, path);
            // metadata changed - need to broadcast an update
            base_.send_changed(event_group_, this);
            send(event_group_, utility::event_atom_v, change_atom_v);
        },

        [=](json_store::set_json_atom atom, const JsonStore &json) {
            delegate(json_store_, atom, json);
            // metadata changed - need to broadcast an update
            base_.send_changed(event_group_, this);
            send(event_group_, utility::event_atom_v, change_atom_v);
        },

        [=](json_store::merge_json_atom atom, const JsonStore &json) {
            delegate(json_store_, atom, json);
            // metadata changed - need to broadcast an update
            base_.send_changed(event_group_, this);
            send(event_group_, utility::event_atom_v, change_atom_v);
        },

        [=](json_store::set_json_atom atom, const JsonStore &json, const std::string &path) {
            delegate(json_store_, atom, json, path);
            // metadata changed - need to broadcast an update
            base_.send_changed(event_group_, this);
            send(event_group_, utility::event_atom_v, change_atom_v);
        },

        [=](media::invalidate_cache_atom) -> caf::result<media::MediaKeyVector> {
            auto rp = make_response_promise<media::MediaKeyVector>();

            // build list of our possible cache keys..
            request(caf::actor_cast<actor>(this), infinite, media_cache::keys_atom_v)
                .then(
                    [=](const media::MediaKeyVector &keys) mutable {
                        auto image_cache =
                            system().registry().template get<caf::actor>(image_cache_registry);
                        auto audio_cache =
                            system().registry().template get<caf::actor>(audio_cache_registry);
                        std::vector<caf::actor> caches;

                        if (image_cache)
                            caches.push_back(image_cache);
                        if (audio_cache)
                            caches.push_back(audio_cache);
                        if (caches.empty()) {
                            rp.deliver(media::MediaKeyVector());
                            return;
                        }

                        fan_out_request<policy::select_all>(
                            caches, infinite, media_cache::erase_atom_v, keys)
                            .then(
                                [=](std::vector<media::MediaKeyVector> erased_keys) mutable {
                                    media::MediaKeyVector result;
                                    for (const auto &i : erased_keys)
                                        result.insert(result.end(), i.begin(), i.end());
                                    rp.deliver(result);
                                },
                                [=](error &err) mutable { rp.deliver(std::move(err)); });
                    },
                    [=](error &err) mutable { rp.deliver(std::move(err)); });

            return rp;
        },

        [=](media_cache::keys_atom) -> caf::result<MediaKeyVector> {
            auto rp = make_response_promise<MediaKeyVector>();
            deliver_frames_media_keys(rp, MT_IMAGE, std::vector<int>());
            return rp;
        },

        [=](media_cache::keys_atom, const MediaType media_type) -> caf::result<MediaKeyVector> {
            auto rp = make_response_promise<MediaKeyVector>();
            deliver_frames_media_keys(rp, media_type, std::vector<int>());
            return rp;
        },

        [=](media_cache::keys_atom,
            const MediaType media_type,
            const int logical_frame) -> caf::result<MediaKey> {
            auto rp = make_response_promise<MediaKey>();

            request(
                caf::actor_cast<actor>(this),
                infinite,
                media_cache::keys_atom_v,
                media_type,
                std::vector<int>({logical_frame}))
                .then(
                    [=](const MediaKeyVector &r) mutable {
                        if (r.size()) {
                            rp.deliver(r[0]);
                        } else {
                            rp.deliver(make_error(xstudio_error::error, "No keys for frames"));
                        }
                    },
                    [=](error &err) mutable { rp.deliver(std::move(err)); });

            return rp;
        },

        [=](media_cache::keys_atom,
            const MediaType media_type,
            const std::vector<int> &logical_frames) -> caf::result<MediaKeyVector> {
            auto rp = make_response_promise<MediaKeyVector>();
            deliver_frames_media_keys(rp, media_type, logical_frames);
            return rp;
        },

        [=](media_hook::get_media_hook_atom) -> caf::result<bool> {
            auto rp      = make_response_promise<bool>();
            auto m_actor = system().registry().template get<caf::actor>(media_hook_registry);

            if (not m_actor) {
                rp.deliver(false);
            } else {
                request(m_actor, infinite, media_hook::get_media_hook_atom_v, this)
                    .then(
                        [=](const bool done) mutable { rp.deliver(done); },
                        [=](error &err) mutable { rp.deliver(std::move(err)); });
            }

            return rp;
        },

        [=](media_metadata::get_metadata_atom) -> caf::result<bool> {
            auto m_actor =
                system().registry().template get<caf::actor>(media_metadata_registry);
            if (not m_actor)
                return caf::result<bool>(false);

            auto rp = make_response_promise<bool>();

            try {
                if (not base_.media_reference().container()) {
                    int file_frame;
                    auto first_uri = base_.media_reference().uri(0, file_frame);

                    // #pragma message "Currently only reading metadata on first frame for image
                    // sequences"

                    // If we read metadata for every frame the whole app grinds when inspecting
                    // big or multiple sequences
                    if (first_uri) {

                        request(m_actor, infinite, get_metadata_atom_v, *first_uri, file_frame)
                            .then(
                                [=](const std::pair<JsonStore, int> &meta) mutable {
                                    request(
                                        json_store_,
                                        infinite,
                                        json_store::set_json_atom_v,
                                        meta.first,
                                        "/metadata/media/@" + std::to_string(meta.second),
                                        true)
                                        .then(
                                            [=](const bool &done) mutable {
                                                rp.deliver(done);
                                                // notify any watchers that metadata is updated
                                                send(
                                                    event_group_,
                                                    utility::event_atom_v,
                                                    get_metadata_atom_v,
                                                    meta.first);
                                            },
                                            [=](error &err) mutable {
                                                rp.deliver(std::move(err));
                                            });
                                },
                                [=](error &err) mutable { rp.deliver(std::move(err)); });
                    } else {
                        rp.deliver(make_error(
                            xstudio_error::error,
                            std::string("Sequence with no frames ") +
                                to_string(base_.media_reference().uri())));
                    }

                    /*for(size_t i=0;i<frames.size();i++) {
                            request(m_actor, infinite, get_metadata_atom_v, frames[i].first,
                    frames[i].second).then(
                                    [=] (const std::pair<JsonStore, int> &meta) mutable {
                                            request(json_store_, infinite,
                    json_store::set_json_atom_v, meta.first,
                    "/metadata/media/@"+std::to_string(meta.second), true).then(
                                                    [=] (const bool &done) mutable {
                                                            if(i == frames.size()-1) {
                                                                    rp.deliver(done);
                                                                    // notify any watchers that
                    metadata is updated send(event_group_, utility::event_atom_v,
                    get_metadata_atom_v, meta.first);
                                                            }
                                                    },
                                            [=](error& err) mutable {
                                                            if(i == frames.size()-1)
                                                            rp.deliver(std::move(err));
                                            }
                                            );
                                    },
                            [=](error& err) mutable {
                                    rp.deliver(std::move(err));
                            }
                            );
                    }*/

                } else {
                    request(
                        m_actor, infinite, get_metadata_atom_v, base_.media_reference().uri())
                        .then(
                            [=](const std::pair<JsonStore, int> &meta) mutable {
                                request(
                                    json_store_,
                                    infinite,
                                    json_store::set_json_atom_v,
                                    meta.first,
                                    "/metadata/media/@")
                                    .then(
                                        [=](const bool &done) mutable {
                                            rp.deliver(done);
                                            // notify any watchers that metadata is updated
                                            send(
                                                event_group_,
                                                utility::event_atom_v,
                                                get_metadata_atom_v,
                                                meta.first);
                                        },
                                        [=](error &err) mutable {
                                            rp.deliver(std::move(err));
                                        });
                            },
                            [=](error &err) mutable { rp.deliver(std::move(err)); });
                }
            } catch (const std::exception &e) {
                rp.deliver(make_error(
                    xstudio_error::error,
                    std::string("media_metadata::get_metadata_atom ") + e.what()));
            }

            return rp;
        },

        [=](media_metadata::get_metadata_atom, const int sequence_frame) -> caf::result<bool> {
            if (base_.media_reference().container())
                return make_error(xstudio_error::error, "Media has no frames");

            auto _uri = base_.media_reference().uri_from_frame(sequence_frame);
            if (not _uri)
                return make_error(xstudio_error::error, "Invalid frame index");

            auto rp = make_response_promise<bool>();
            auto m_actor =
                system().registry().template get<caf::actor>(media_metadata_registry);
            request(m_actor, infinite, get_metadata_atom_v, *_uri)
                .then(
                    [=](const std::pair<JsonStore, int> &meta) mutable {
                        request(
                            json_store_,
                            infinite,
                            json_store::set_json_atom_v,
                            meta.first,
                            "/metadata/media/@" + std::to_string(meta.second))
                            .then(
                                [=](const bool &done) mutable { rp.deliver(done); },
                                [=](error &err) mutable { rp.deliver(std::move(err)); });
                    },
                    [=](error &err) mutable { rp.deliver(std::move(err)); });
            return rp;
        },

        [=](utility::duplicate_atom) -> result<UuidActor> {
            auto rp    = make_response_promise<UuidActor>();
            auto uuid  = utility::Uuid::generate();
            auto actor = spawn<MediaSourceActor>(
                base_.name(), base_.reader(), base_.media_reference(), uuid);

            // using a lambda to try and make this more, err, 'readable'
            auto copy_metadata = [=](UuidActor destination,
                                     caf::typed_response_promise<UuidActor> rp) {
                request(json_store_, infinite, json_store::get_json_atom_v)
                    .then(
                        [=](const JsonStore &meta) mutable {
                            request(
                                destination.actor(),
                                infinite,
                                json_store::set_json_atom_v,
                                meta)
                                .then(
                                    [=](bool) mutable { rp.deliver(destination); },
                                    [=](const error &err) mutable { rp.deliver(err); });
                        },
                        [=](const error &err) mutable { rp.deliver(err); });
            };

            // duplicate streams..
            if (media_streams_.size()) {
                auto source_count = std::make_shared<int>();
                (*source_count)   = media_streams_.size();
                for (auto p : media_streams_) {
                    request(p.second, infinite, utility::duplicate_atom_v)
                        .await(
                            [=](UuidActor stream) mutable {
                                // add the stream to the duplicated media_source_actor
                                request(actor, infinite, add_media_stream_atom_v, stream)
                                    .await(
                                        [=](UuidActor) mutable {
                                            // set the current stream as required
                                            if (p.first == base_.current(MT_IMAGE)) {

                                                anon_send(
                                                    actor,
                                                    current_media_stream_atom_v,
                                                    MT_IMAGE,
                                                    stream.uuid());

                                            } else if (p.first == base_.current(MT_AUDIO)) {

                                                anon_send(
                                                    actor,
                                                    current_media_stream_atom_v,
                                                    MT_AUDIO,
                                                    stream.uuid());
                                            }

                                            (*source_count)--;
                                            if (!(*source_count)) {
                                                copy_metadata(UuidActor(uuid, actor), rp);
                                            }
                                        },
                                        [=](const error &err) mutable { rp.deliver(err); });
                            },
                            [=](const error &err) mutable { rp.deliver(err); });
                }
            } else {
                copy_metadata(UuidActor(uuid, actor), rp);
            }
            return rp;
        },

        [=](utility::event_atom, utility::change_atom) {},

        [=](utility::event_atom, utility::name_atom, const std::string & /*name*/) {},

        [=](utility::get_group_atom _get_group_atom) {
            delegate(json_store_, _get_group_atom);
        },

        [=](utility::parent_atom) -> caf::actor { return actor_cast<actor>(parent_); },

        [=](utility::parent_atom, const UuidActor &parent) {
            parent_uuid_ = parent.uuid();
            parent_      = actor_cast<actor_addr>(parent.actor());
            base_.send_changed(event_group_, this);
        },

        // deprecated
        [=](utility::parent_atom, caf::actor parent) {
            request(parent, infinite, utility::uuid_atom_v)
                .then(
                    [=](const utility::Uuid &parent_uuid) { parent_uuid_ = parent_uuid; },
                    ERR_HANDLER_FUNC);
            parent_ = actor_cast<actor_addr>(parent);
            base_.send_changed(event_group_, this);
        },

        [=](utility::serialise_atom) -> result<JsonStore> {
            auto rp = make_response_promise<JsonStore>();

            request(json_store_, infinite, json_store::get_json_atom_v, "")
                .then(
                    [=](const JsonStore &meta) mutable {
                        std::vector<caf::actor> clients;

                        for (const auto &i : media_streams_)
                            clients.push_back(i.second);

                        if (not clients.empty()) {
                            fan_out_request<policy::select_all>(
                                clients, infinite, serialise_atom_v)
                                .then(
                                    [=](std::vector<JsonStore> json) mutable {
                                        JsonStore jsn;
                                        jsn["base"]   = base_.serialise();
                                        jsn["store"]  = meta;
                                        jsn["actors"] = {};
                                        for (const auto &j : json)
                                            jsn["actors"][static_cast<std::string>(
                                                j["base"]["container"]["uuid"])] = j;

                                        rp.deliver(jsn);
                                    },
                                    [=](error &err) mutable { rp.deliver(std::move(err)); });
                        } else {
                            JsonStore jsn;
                            jsn["store"]  = meta;
                            jsn["base"]   = base_.serialise();
                            jsn["actors"] = {};
                            rp.deliver(jsn);
                        }
                    },
                    [=](error &err) mutable { rp.deliver(std::move(err)); });
            return rp;
        });
}


// needs cleaning up as this logic is a bit messy.
void MediaSourceActor::send_source_details_to_ui(caf::actor ui_actor) {
    // do we have metadata for the source yet?
    request(json_store_, infinite, json_store::get_json_atom_v, "/metadata/media")
        .then(
            [=](const JsonStore &meta_data) mutable {
                // Yes! Send it to the media_source_ui actor
                send(ui_actor, utility::event_atom_v, get_metadata_atom_v, meta_data);
            },
            [=](error & /*err*/) mutable {
                // No. Force it to get the metadata
                request(actor_cast<actor>(this), infinite, media_metadata::get_metadata_atom_v)
                    .then(
                        [=](bool) mutable {
                            // We're guaranteed to have the metadata now. Send it to the
                            // media_source_ui actor
                            anon_send(
                                json_store_, json_store::get_json_atom_v, "/colour_pipeline");

                            request(
                                json_store_,
                                infinite,
                                json_store::get_json_atom_v,
                                "/metadata/media")
                                .then(
                                    [=](const JsonStore &meta_data) mutable {
                                        send(
                                            ui_actor,
                                            utility::event_atom_v,
                                            get_metadata_atom_v,
                                            meta_data);
                                    },
                                    ERR_HANDLER_FUNC);
                        },
                        [=](error &err) mutable {
                            // failed to get metadata..
                            // send empty..
                            // invalid media path.. ?
                            send(
                                ui_actor,
                                utility::event_atom_v,
                                get_metadata_atom_v,
                                JsonStore());
                            spdlog::warn("{} {}", __PRETTY_FUNCTION__, to_string(err));
                        });
            });

    anon_send(ui_actor, utility::detail_atom_v, base_.detail(this, event_group_));
}

void MediaSourceActor::send_stream_details_to_ui(caf::actor ui_actor) {
    static const std::regex as_hash_pad(R"(\{:0(\d+)d\})");

    // Gather details about the media stream into a tuple and send in
    // one go to the MediaSourceUI actor. This means we can initialise
    // the MediaSourceUI without blocking the UI thread while waiting
    // for this actor to fetch this data.

    // shouldn't really be passing filename here..
    // that's a source property not a stream one..
    const auto &mr = base_.media_reference();

    auto path            = fs::path(uri_to_posix_path(mr.uri()));
    std::string filename = path.filename();

    if (not mr.container()) {
        try {
            filename = fmt::format(std::regex_replace(filename, as_hash_pad, R"({:#<$1})"), "");
        } catch (const std::exception &e) {
            filename = fmt::format(std::regex_replace(filename, as_hash_pad, R"(#)"), "");
        }
    }
    path.replace_filename(filename);

    std::string fps_string = fmt::format("{:.3f}", mr.rate().to_fps());
    fps_string             = fps_string.substr(0, fps_string.find_last_not_of('0') + 1);
    if (fps_string.find(".") == (fps_string.length() - 1))
        fps_string = fps_string + "0";

    std::vector<UuidActor> stream_actors;
    for (const auto &i : base_.streams(MT_IMAGE))
        stream_actors.emplace_back(UuidActor(i, media_streams_.at(i)));

    if (media_streams_.empty()) {
        // Invalid media..
        anon_send(
            ui_actor,
            std::tuple<
                utility::Uuid,
                std::string,
                std::string,
                double,
                StreamDetail,
                std::vector<UuidActor>,
                Uuid>(
                base_.uuid(),
                path,
                fps_string,
                mr.rate().to_fps(),
                StreamDetail(),
                stream_actors,
                base_.current(MT_IMAGE)));


    } else
        request(
            media_streams_.at(base_.current(media::MT_IMAGE)),
            infinite,
            get_stream_detail_atom_v)
            .then(
                [=](const StreamDetail &stream_detail) mutable {
                    anon_send(
                        ui_actor,
                        std::tuple<
                            utility::Uuid,
                            std::string,
                            std::string,
                            double,
                            StreamDetail,
                            std::vector<UuidActor>,
                            Uuid>(
                            base_.uuid(),
                            path,
                            fps_string,
                            mr.rate().to_fps(),
                            stream_detail,
                            stream_actors,
                            base_.current(MT_IMAGE)));
                },
                ERR_HANDLER_FUNC);
}

void MediaSourceActor::get_media_pointers_for_frames(
    const MediaType media_type,
    const LogicalFrameRanges &ranges,
    caf::typed_response_promise<media::AVFrameIDs> rp) {
    if (base_.current(media_type).is_null()) {
        // in the case where there is no source, return list of empty frames.
        // This is useful for sources that have no audio or no video, to keep
        // them compatible with the video based frame request/deliver playback
        // system
        media::AVFrameIDs result;
        for (const auto &i : ranges) {
            for (auto ii = i.first; ii <= i.second; ii++)
                result.emplace_back(media::make_blank_frame(media_type)
                                    // std::shared_ptr<const media::AVFrameID>(
                                    //     new media::AVFrameID()
                                    //     )
                );
        }
        rp.deliver(result);
        return;
    }

    // get colours params ... only need this for media_type == MT_IMAGE though
    request(json_store_, infinite, json_store::get_json_atom_v, "/colour_pipeline")
        .then(
            [=](const JsonStore &meta) mutable {
                request(
                    media_streams_.at(base_.current(media_type)),
                    infinite,
                    get_stream_detail_atom_v)
                    .then(
                        [=](const StreamDetail &detail) mutable {
                            media::AVFrameIDs result;
                            media::AVFrameID mptr;

                            for (const auto &i : ranges) {
                                for (auto logical_frame = i.first; logical_frame <= i.second;
                                     logical_frame++) {
                                    // the try block catches posible 'out_of_range'
                                    // exception coming from MediaReference::uri()
                                    try {

                                        int frame;
                                        auto _uri =
                                            base_.media_reference().uri(logical_frame, frame);

                                        if (not _uri)
                                            throw std::runtime_error("Time out of range");

                                        if (mptr.is_nil()) {
                                            mptr = media::AVFrameID(
                                                *_uri,
                                                frame,
                                                *(base_.media_reference().frame(0)),
                                                base_.media_reference().rate(),
                                                detail.name_,
                                                detail.key_format_,
                                                base_.reader(),
                                                caf::actor_cast<caf::actor_addr>(this),
                                                meta,
                                                base_.current(media_type),
                                                parent_uuid_,
                                                media_type);
                                        } else {
                                            mptr.uri_   = *_uri;
                                            mptr.frame_ = frame;
                                            mptr.key_   = media::MediaKey(
                                                detail.key_format_, *_uri, frame, detail.name_);
                                        }

                                        result.emplace_back(
                                            std::shared_ptr<const media::AVFrameID>(
                                                new media::AVFrameID(mptr)));
                                    } catch (const std::exception &e) {
                                        result.emplace_back(
                                            media::make_blank_frame(media_type));
                                    }
                                }
                            }
                            rp.deliver(result);
                        },
                        [=](error &err) mutable { rp.deliver(std::move(err)); });
            },
            [=](error &err) mutable { rp.deliver(std::move(err)); });
}

void MediaSourceActor::deliver_frames_media_keys(
    caf::typed_response_promise<media::MediaKeyVector> rp,
    const MediaType media_type,
    const std::vector<int> logical_frames) {
    if (base_.empty()) {
        if (base_.error_detail().empty()) {
            rp.deliver(make_error(xstudio_error::error, "No MediaStreams"));
        } else {
            rp.deliver(make_error(xstudio_error::error, base_.error_detail()));
        }
        return;
    }

    auto stream = base_.current(media_type);
    if (stream.is_null()) {
        rp.deliver(make_error(xstudio_error::error, "No Stream for MediaType"));
        return;
    }

    request(media_streams_.at(stream), infinite, get_stream_detail_atom_v)
        .then(
            [=](const StreamDetail &detail) mutable {
                MediaKeyVector result;
                if (logical_frames.empty()) {

                    // if logical frames is empty, we return keys for ALL the frames in the
                    // source frame range
                    auto uris = base_.media_reference().uris();
                    for (const auto &u : uris) {
                        result.emplace_back(
                            MediaKey(detail.key_format_, u.first, u.second, detail.name_));
                    }

                } else {

                    for (const int logical_frame : logical_frames) {
                        int frame;
                        try {
                            auto _uri = base_.media_reference().uri(logical_frame, frame);
                            if (not _uri)
                                throw std::runtime_error("Time out of range");

                            result.emplace_back(
                                MediaKey(detail.key_format_, *_uri, frame, detail.name_));
                        } catch (...) {
                            result.emplace_back(MediaKey());
                        }
                    }
                }
                rp.deliver(result);
            },
            [=](error &err) mutable { rp.deliver(std::move(err)); });
}
