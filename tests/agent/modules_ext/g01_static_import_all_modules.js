// modules_ext g01 (dynajs-only; CONFIG_NATIVE_MODULES=y): STATIC import of all 34
// portable dyna: modules; print namespace typeof + export-name counts (sorted,
// byte-stable). Parse-time absence of any module aborts the whole file -> visible.
import * as m_bytes from "dyna:bytes";
import * as m_cli from "dyna:cli";
import * as m_compress from "dyna:compress";
import * as m_config from "dyna:config";
import * as m_crypto from "dyna:crypto";
import * as m_csv from "dyna:csv";
import * as m_dataframe from "dyna:dataframe";
import * as m_decimal from "dyna:decimal";
import * as m_encoding from "dyna:encoding";
import * as m_file from "dyna:file";
import * as m_hash from "dyna:hash";
import * as m_html from "dyna:html";
import * as m_http from "dyna:http";
import * as m_json from "dyna:json";
import * as m_log from "dyna:log";
import * as m_matcher from "dyna:matcher";
import * as m_mathx from "dyna:mathx";
import * as m_ml from "dyna:ml";
import * as m_net from "dyna:net";
import * as m_random from "dyna:random";
import * as m_schema from "dyna:schema";
import * as m_scrape from "dyna:scrape";
import * as m_semver from "dyna:semver";
import * as m_serialize from "dyna:serialize";
import * as m_simd from "dyna:simd";
import * as m_structures from "dyna:structures";
import * as m_sys from "dyna:sys";
import * as m_time from "dyna:time";
import * as m_url from "dyna:url";
import * as m_uuid from "dyna:uuid";
import * as m_validate from "dyna:validate";
import * as m_xml from "dyna:xml";
import * as m_yaml from "dyna:yaml";
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const mods = { bytes: m_bytes, cli: m_cli, compress: m_compress, config: m_config, crypto: m_crypto, csv: m_csv, dataframe: m_dataframe, decimal: m_decimal, encoding: m_encoding, file: m_file, hash: m_hash, html: m_html, http: m_http, json: m_json, log: m_log, matcher: m_matcher, mathx: m_mathx, ml: m_ml, net: m_net, random: m_random, schema: m_schema, scrape: m_scrape, semver: m_semver, serialize: m_serialize, simd: m_simd, structures: m_structures, sys: m_sys, time: m_time, url: m_url, uuid: m_uuid, validate: m_validate, xml: m_xml, yaml: m_yaml };
for (const name of Object.keys(mods).sort()) {
  const m = mods[name];
  const keys = Object.keys(m).sort();
  out(name + ': typeof=' + (typeof m) + ' exports=' + keys.length + ' first=' + (keys[0] || '-'));
}
out('DONE');
