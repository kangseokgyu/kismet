/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#include "config.h"

#include "phy_rtl433.h"
#include "devicetracker.h"
#include "endian_magic.h"
#include "macaddr.h"
#include "kis_httpd_registry.h"
#include "manuf.h"
#include "messagebus.h"

Kis_RTL433_Phy::Kis_RTL433_Phy(int in_phyid) :
    kis_phy_handler(in_phyid) {

    set_phy_name("RTL433");

    packetchain =
        Globalreg::fetch_mandatory_global_as<packet_chain>();
    entrytracker =
        Globalreg::fetch_mandatory_global_as<entry_tracker>();
    devicetracker = 
        Globalreg::fetch_mandatory_global_as<device_tracker>();

	pack_comp_common = 
		packetchain->register_packet_component("COMMON");
    pack_comp_json = 
        packetchain->register_packet_component("JSON");
    pack_comp_meta =
        packetchain->register_packet_component("METABLOB");

    rtl433_holder_id =
        Globalreg::globalreg->entrytracker->register_field("rtl433.device", 
                tracker_element_factory<tracker_element_map>(),
                "rtl_433 device");

    rtl433_common_id =
        Globalreg::globalreg->entrytracker->register_field("rtl433.device.common",
                tracker_element_factory<rtl433_tracked_common>(),
                "Common RTL433 device info");

    rtl433_thermometer_id =
        Globalreg::globalreg->entrytracker->register_field("rtl433.device.thermometer",
                tracker_element_factory<rtl433_tracked_thermometer>(),
                "RTL433 thermometer");

    rtl433_tpms_id =
        Globalreg::globalreg->entrytracker->register_field("rtl433.device.tpms",
                tracker_element_factory<rtl433_tracked_tpms>(),
                "RTL433 TPMS tire pressure");
    
    rtl433_weatherstation_id =
        Globalreg::globalreg->entrytracker->register_field("rtl433.device.weatherstation",
                tracker_element_factory<rtl433_tracked_weatherstation>(),
                "RTL433 weather station");

    rtl433_switch_id =
        Globalreg::globalreg->entrytracker->register_field("rtl433.device.switch",
                tracker_element_factory<rtl433_tracked_switch>(),
                "RTL433 power switch");

    rtl433_lightning_id =
        Globalreg::globalreg->entrytracker->register_field("rtl433.device.lightningsensor",
                tracker_element_factory<rtl433_tracked_lightningsensor>(),
                "RTL433 lightning sensor");

    // Make the manuf string
    rtl_manuf = Globalreg::globalreg->manufdb->make_manuf("RTL433");

    // Register js module for UI
    auto httpregistry =
        Globalreg::fetch_mandatory_global_as<kis_httpd_registry>();
    httpregistry->register_js_module("kismet_ui_rtl433", "js/kismet.ui.rtl433.js");

	packetchain->register_handler(&PacketHandler, this, CHAINPOS_CLASSIFIER, -100);
}

Kis_RTL433_Phy::~Kis_RTL433_Phy() {
    packetchain->remove_handler(&PacketHandler, CHAINPOS_CLASSIFIER);
}

double Kis_RTL433_Phy::f_to_c(double f) {
    return (f - 32) / (double) 1.8f;
}

mac_addr Kis_RTL433_Phy::json_to_mac(Json::Value json) {
    // Derive a mac addr from the model and device id data
    //
    // We turn the model string into 4 bytes using the adler32 checksum,
    // then we use the model as a (potentially) 16bit int
    //
    // Finally we set the locally assigned bit on the first octet

    uint8_t bytes[6];
    uint16_t *model = (uint16_t *) bytes;
    uint32_t *checksum = (uint32_t *) (bytes + 2);

    memset(bytes, 0, 6);

    std::string smodel = "unk";

    if (json.isMember("model")) {
        Json::Value m = json["model"];
        if (m.isString()) {
            smodel = m.asString();
        }
    }

    *checksum = adler32_checksum(smodel.c_str(), smodel.length());

    bool set_model = false;
    if (json.isMember("id")) {
        Json::Value i = json["id"];
        if (i.isNumeric()) {
            *model = kis_hton16((uint16_t) i.asUInt());
            set_model = true;
        }
    }

    if (!set_model && json.isMember("device")) {
        Json::Value d = json["device"];
        if (d.isNumeric()) {
            *model = kis_hton16((uint16_t) d.asUInt());
            set_model = true;
        }
    }

    if (!set_model) {
        *model = 0x0000;
    }

    // Set the local bit
    bytes[0] |= 0x2;

    return mac_addr(bytes, 6);
}

bool Kis_RTL433_Phy::json_to_rtl(Json::Value json, kis_packet *packet) {
    std::string err;
    std::string v;

    // synth a mac out of it
    mac_addr rtlmac = json_to_mac(json);

    if (rtlmac.state.error) {
        return false;
    }

    kis_common_info *common = 
        (kis_common_info *) packet->fetch(pack_comp_common);

    if (common == NULL) {
        common = new kis_common_info;
        packet->insert(pack_comp_common, common);
    }

    common->type = packet_basic_data;
    common->phyid = fetch_phy_id();
    common->datasize = 0;

    // If this json record has a channel
    if (json.isMember("channel")) {
        Json::Value c = json["channel"];
        if (c.isNumeric()) {
            common->channel = int_to_string(c.asInt());
        } else if (c.isString()) {
            common->channel = munge_to_printable(c.asString());
        }
    }

    common->freq_khz = 433920;
    common->source = rtlmac;
    common->transmitter = rtlmac;

    std::shared_ptr<kis_tracked_device_base> basedev =
        devicetracker->update_common_device(common, common->source, this, packet,
                (UCD_UPDATE_FREQUENCIES | UCD_UPDATE_PACKETS | UCD_UPDATE_LOCATION |
                 UCD_UPDATE_SEENBY), "RTL433 Sensor");

    kis_lock_guard<kis_mutex> lk(devicetracker->get_devicelist_mutex(), "rtl433_json_to_rtl");

    std::string dn = "Sensor";

    if (json.isMember("model")) {
        dn = munge_to_printable(json["model"].asString());
    }

    basedev->set_manuf(rtl_manuf);

    basedev->set_tracker_type_string(devicetracker->get_cached_devicetype("Sensor"));
    basedev->set_devicename(dn);

    auto rtlholder = basedev->get_sub_as<tracker_element_map>(rtl433_holder_id);
    bool newrtl = false;

    if (rtlholder == NULL) {
        rtlholder =
            std::make_shared<tracker_element_map>(rtl433_holder_id);
        basedev->insert(rtlholder);
        newrtl = true;
    }

    auto commondev =
        rtlholder->get_sub_as<rtl433_tracked_common>(rtl433_common_id);

    if (commondev == NULL) {
        commondev =
            std::make_shared<rtl433_tracked_common>(rtl433_common_id);
        rtlholder->insert(commondev);

        commondev->set_model(dn);

        bool set_id = false;
        if (json.isMember("id")) {
            Json::Value id_j = json["id"];
            if (id_j.isNumeric()) {
                std::stringstream ss;
                ss << id_j.asUInt64();
                commondev->set_rtlid(ss.str());
                set_id = true;
            } else if (id_j.isString()) {
                commondev->set_rtlid(id_j.asString());
                set_id = true;
            }
        }

        if (!set_id && json.isMember("device")) {
            Json::Value device_j = json["device"];
            if (device_j.isNumeric()) {
                std::stringstream ss;
                ss << device_j.asUInt64();
                commondev->set_rtlid(ss.str());
                set_id = true;
            } else if (device_j.isString()) {
                commondev->set_rtlid(device_j.asString());
                set_id = true;
            }
        }

        if (!set_id) {
            commondev->set_rtlid("");
        }

        commondev->set_rtlchannel("0");
    }

    if (json.isMember("channel")) {
        auto channel_j = json["channel"];

        if (channel_j.isNumeric())
            commondev->set_rtlchannel(int_to_string(channel_j.asInt()));
        else if (channel_j.isString())
            commondev->set_rtlchannel(munge_to_printable(channel_j.asString()));
    }

    if (json.isMember("battery")) {
        auto battery_j = json["battery"];

        if (battery_j.isString())
            commondev->set_battery(munge_to_printable(battery_j.asString()));
    }

    if (is_thermometer(json))
        add_thermometer(json, rtlholder);

    if (is_weather_station(json))
        add_weather_station(json, rtlholder);

    if (is_tpms(json))
        add_tpms(json, rtlholder);

    if (is_switch(json))
        add_switch(json, rtlholder);

    if (is_lightning(json))
        add_lightning(json, rtlholder);

    if (newrtl && commondev != NULL) {
        std::string info = "Detected new RTL433 RF device '" + commondev->get_model() + "'";

        if (commondev->get_rtlid() != "") 
            info += " ID " + commondev->get_rtlid();

        if (commondev->get_rtlchannel() != "0")
            info += " Channel " + commondev->get_rtlchannel();

        _MSG(info, MSGFLAG_INFO);
    }

    return true;
}

bool Kis_RTL433_Phy::is_weather_station(Json::Value json) {
    auto direction_j = json["direction_deg"];
    auto windstrength_j = json["windstrength"];
    auto winddirection_j = json["winddirection"];
    auto windspeed_j = json["speed"];
    auto gust_j = json["gust"];
    auto rain_j = json["rain"];
    auto uv_index_j = json["uv_index"];
    auto lux_j = json["lux"];

    if (!direction_j.isNull() || !windstrength_j.isNull() || !winddirection_j.isNull() ||
            !windspeed_j.isNull() || !gust_j.isNull() || !rain_j.isNull() || !uv_index_j.isNull() ||
            !lux_j.isNull()) {
        return true;
    }

    return false;
}

bool Kis_RTL433_Phy::is_thermometer(Json::Value json) {
    auto humidity_j = json["humidity"];
    auto moisture_j = json["moisture"];
    auto temp_f_j = json["temperature_F"];
    auto temp_c_j = json["temperature_C"];

    if (!humidity_j.isNull() || !moisture_j.isNull() || !temp_f_j.isNull() || !temp_c_j.isNull()) {
        return true;
    }

    return false;
}

bool Kis_RTL433_Phy::is_tpms(Json::Value json) {
    auto type_j = json["type"];

    if (type_j.isString() && type_j.asString() == "TPMS")
        return true;

    return false;
}

bool Kis_RTL433_Phy::is_switch(Json::Value json) {
    auto sw0_j = json["switch0"];
    auto sw1_j = json["switch1"];

    if (!sw0_j.isNull() || !sw1_j.isNull())
        return true;

    return false;
}

bool Kis_RTL433_Phy::is_lightning(Json::Value json) {
    auto strike_j = json["strike_count"];
    auto storm_j = json["storm_dist"];
    auto active_j = json["active"];
    auto rfi_j = json["rfi"];

    if (strike_j.isNull() || storm_j.isNull() || active_j.isNull() || rfi_j.isNull()) 
        return false;

    return true;
}

void Kis_RTL433_Phy::add_weather_station(Json::Value json, 
        std::shared_ptr<tracker_element_map> rtlholder) {
    auto direction_j = json["direction_deg"];
    auto windstrength_j = json["windstrength"];
    auto wind_avg_km_j = json["wind_avg_km_h"];
    auto winddirection_j = json["winddirection"];
    auto windspeed_j = json["speed"];
    auto gust_j = json["gust"];
    auto rain_j = json["rain"];
    auto uv_index_j = json["uv_index"];
    auto lux_j = json["lux"];

    if (!direction_j.isNull() || !windstrength_j.isNull() || !winddirection_j.isNull() ||
            !windspeed_j.isNull() || !gust_j.isNull() || !rain_j.isNull() || !uv_index_j.isNull() ||
            !lux_j.isNull() || !wind_avg_km_j.isNull()) {

        auto weatherdev = 
            rtlholder->get_sub_as<rtl433_tracked_weatherstation>(rtl433_weatherstation_id);

        if (weatherdev == NULL) {
            weatherdev = 
                std::make_shared<rtl433_tracked_weatherstation>(rtl433_weatherstation_id);
            rtlholder->insert(weatherdev);
        }

        if (direction_j.isNumeric()) {
            weatherdev->set_wind_dir(direction_j.asInt());
            weatherdev->get_wind_dir_rrd()->add_sample(direction_j.asInt(), time(0));
        }

        if (winddirection_j.isNumeric()) {
            weatherdev->set_wind_dir(winddirection_j.asInt());
            weatherdev->get_wind_dir_rrd()->add_sample(winddirection_j.asInt(), time(0));
        }

        if (windspeed_j.isNumeric()) {
            weatherdev->set_wind_speed((int32_t) windspeed_j.asInt());
            weatherdev->get_wind_speed_rrd()->add_sample((int64_t) windspeed_j.asInt(), time(0));
        }

        if (wind_avg_km_j.isNumeric()) {
            weatherdev->set_wind_speed((int32_t) wind_avg_km_j.asInt());
            weatherdev->get_wind_speed_rrd()->add_sample((int64_t) wind_avg_km_j.asInt(), time(0));
        }

        if (windstrength_j.isNumeric()) {
            weatherdev->set_wind_speed((int32_t) windstrength_j.asInt());
            weatherdev->get_wind_speed_rrd()->add_sample((int64_t) windstrength_j.asInt(),
                    time(0));
        }

        if (gust_j.isNumeric()) {
            weatherdev->set_wind_gust((int32_t) gust_j.asInt());
            weatherdev->get_wind_gust_rrd()->add_sample((int64_t) gust_j.asInt(), time(0));
        }

        if (rain_j.isNumeric()) {
            weatherdev->set_rain((int32_t) rain_j.asInt());
            weatherdev->get_rain_rrd()->add_sample((int64_t) rain_j.asInt(), time(0));
        }

        if (uv_index_j.isNumeric()) {
            weatherdev->set_uv_index((int32_t) uv_index_j.asInt());
            weatherdev->get_uv_index_rrd()->add_sample((int64_t) uv_index_j.asInt(), time(0));
        }

        if (lux_j.isNumeric()) {
            weatherdev->set_lux((int32_t) lux_j.asInt());
            weatherdev->get_lux_rrd()->add_sample((int64_t) lux_j.asInt(), time(0));
        }

    }
}

void Kis_RTL433_Phy::add_thermometer(Json::Value json, std::shared_ptr<tracker_element_map> rtlholder) {
    auto humidity_j = json["humidity"];
    auto moisture_j = json["moisture"];
    auto temp_f_j = json["temperature_F"];
    auto temp_c_j = json["temperature_C"];

    if (!humidity_j.isNull() || !moisture_j.isNull() || !temp_f_j.isNull() || !temp_c_j.isNull()) {
        auto thermdev = 
            rtlholder->get_sub_as<rtl433_tracked_thermometer>(rtl433_thermometer_id);

        if (thermdev == NULL) {
            thermdev = 
                std::make_shared<rtl433_tracked_thermometer>(rtl433_thermometer_id);
            rtlholder->insert(thermdev);
        }

        if (humidity_j.isNumeric()) {
            thermdev->set_humidity(humidity_j.asInt());
        }

        if (moisture_j.isNumeric()) {
            thermdev->set_humidity(moisture_j.asInt());
        }

        if (temp_f_j.isNumeric()) {
            thermdev->set_temperature(f_to_c(temp_f_j.asInt()));
        }

        if (temp_c_j.isNumeric()) {
            thermdev->set_temperature(temp_c_j.asInt());
        }
    }
}

void Kis_RTL433_Phy::add_tpms(Json::Value json, std::shared_ptr<tracker_element_map> rtlholder) {
    auto type_j = json["type"];
    auto pressure_j = json["pressure_bar"];
    auto pressurekpa_j = json["pressure_kPa"];
    auto flags_j = json["flags"];
    auto checksum_j = json["mic"];
    auto state_j = json["state"];
    auto code_j = json["code"];

    if (type_j.isString() && type_j.asString() == "TPMS") {
        auto tpmsdev = 
            rtlholder->get_sub_as<rtl433_tracked_tpms>(rtl433_tpms_id);

        if (tpmsdev == NULL) {
            tpmsdev = 
                std::make_shared<rtl433_tracked_tpms>(rtl433_tpms_id);
            rtlholder->insert(tpmsdev);
        }

        if (pressure_j.isNumeric()) {
            tpmsdev->set_pressure_bar(pressure_j.asDouble());
        }

        if (pressurekpa_j.isNumeric()) {
            tpmsdev->set_pressure_kpa(pressurekpa_j.asDouble());
        }

        if (flags_j.isString()) {
            tpmsdev->set_flags(flags_j.asString());
        }

        if (checksum_j.isString()) {
            tpmsdev->set_checksum(checksum_j.asString());
        }

        if (state_j.isString()) {
            tpmsdev->set_state(state_j.asString());
        }

        if (code_j.isString()) {
            tpmsdev->set_code(code_j.asString());
        }

    }

}

void Kis_RTL433_Phy::add_switch(Json::Value json, std::shared_ptr<tracker_element_map> rtlholder) {
    auto sw0_j = json["switch0"];
    auto sw1_j = json["switch1"];

    if (!sw0_j.isNull() || !sw1_j.isNull()) {
        auto switchdev = 
            rtlholder->get_sub_as<rtl433_tracked_switch>(rtl433_switch_id);

        if (switchdev == NULL) {
            switchdev = 
                std::make_shared<rtl433_tracked_switch>(rtl433_switch_id);
            rtlholder->insert(switchdev);
        }

        int x;
        std::stringstream ss;

        if (!sw0_j.isNull())
            x = 0;
        else
            x = 1;

        switchdev->get_switch_vec()->clear();

        while (1) {
            int v = 0;

            ss.str("");
            ss << "switch" << x;
            x++;

            auto v_j = json[ss.str()];

            if (v_j.isNull())
                break;

            if (v_j.isString()) {
                if (v_j.asString() == "OPEN")
                    v = 1;
            } else if (v_j.isNumeric()) {
                v = v_j.asInt();
            } else {
                break;
            }

            auto e = switchdev->make_switch_entry(v);
            switchdev->get_switch_vec()->push_back(e);
        }
    }
}

void Kis_RTL433_Phy::add_lightning(Json::Value json, std::shared_ptr<tracker_element_map> rtlholder) {
    // {"time" : "2019-02-24 22:12:13", "model" : "Acurite Lightning 6045M", "id" : 15580, "channel" : "B", "temperature_F" : 38.300, "humidity" : 53, "strike_count" : 1, "storm_dist" : 8, "active" : 1, "rfi" : 0, "ussb1" : 0, "battery" : "OK", "exception" : 0, "raw_msg" : "bcdc6f354edb81886e"}
    auto strike_j = json["strike_count"];
    auto storm_j = json["storm_dist"];
    auto active_j = json["active"];
    auto rfi_j = json["rfi"];

    if (strike_j.isNull() || storm_j.isNull() || active_j.isNull() || rfi_j.isNull()) 
        return;

    auto lightningdev = 
        rtlholder->get_sub_as<rtl433_tracked_lightningsensor>(rtl433_lightning_id);

    if (lightningdev == NULL) {
        lightningdev = 
            std::make_shared<rtl433_tracked_lightningsensor>(rtl433_lightning_id);
        rtlholder->insert(lightningdev);
    }

    if (strike_j.isNumeric())
        lightningdev->set_strike_count(strike_j.asUInt64());

    if (storm_j.isNumeric())
        lightningdev->set_storm_distance(storm_j.asUInt64());

    if (active_j.isNumeric())
        lightningdev->set_storm_active(active_j.asUInt());

    if (rfi_j.isNumeric()) 
        lightningdev->set_lightning_rfi(rfi_j.asUInt64());
}

int Kis_RTL433_Phy::PacketHandler(CHAINCALL_PARMS) {
    Kis_RTL433_Phy *rtl433 = (Kis_RTL433_Phy *) auxdata;

    if (in_pack->error || in_pack->filtered || in_pack->duplicate)
        return 0;

    kis_json_packinfo *json = in_pack->fetch<kis_json_packinfo>(rtl433->pack_comp_json);
    if (json == NULL)
        return 0;

    if (json->type != "RTL433")
        return 0;

    std::stringstream ss(json->json_string);
    Json::Value device_json;

    try {
        ss >> device_json;

        // Copy the JSON as the meta field for logging, if it's valid
        if (rtl433->json_to_rtl(device_json, in_pack)) {
            packet_metablob *metablob = in_pack->fetch<packet_metablob>(rtl433->pack_comp_meta);
            if (metablob == NULL) {
                metablob = new packet_metablob("RTL433", json->json_string);
                in_pack->insert(rtl433->pack_comp_meta, metablob);
            }
        }
    } catch (std::exception& e) {
        // fprintf(stderr, "debug - error processing rtl json %s\n", e.what());
        return 0;
    }

    return 1;
}

