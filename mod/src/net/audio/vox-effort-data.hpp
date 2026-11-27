#pragma once

// GENERATED from tools/features/data/vox_lines_female.json with the fnv /
// EFFORT_KINDS semantics of tools/features/vox_lines.py genf (the ext's
// vox_lines_female.h: VXE_LINES[i].h[v] and VXE_RANGE[fac][kind]). Do not edit
// by hand: regenerate from that authoritative, read-only JSON.
//
// The table is in-tree but still lives behind TO1_VOX_EFFORT_DATA, like the
// other generated-data seams. With the macro off the owner returns 0 and its
// callers keep the engine's default gender take; pure rules stay live.

#include "vox-effort-core.hpp"

#include <cstdint>

namespace net::audio::effort::data {

#if defined(TO1_VOX_EFFORT_DATA)

    using Line = uint64_t[core::kVoiceCount];

    inline constexpr Line kLines[135] = {
        { 0xa574d69e9b2d3e27ULL, 0x981b434bb727022eULL, 0x79035d1c2893ddbdULL, 0x77f22d1fe8dc5fb4ULL },   // scr t2r-vox-scrfa-eff-give-01
        { 0xa574d79e9b2d3fdaULL, 0x981b424bb727007bULL, 0x79035a1c2893d8a4ULL, 0x77f2301fe8dc64cdULL },   // scr t2r-vox-scrfa-eff-give-02
        { 0xa574d89e9b2d418dULL, 0x981b414bb726fec8ULL, 0x79035b1c2893da57ULL, 0x77f22f1fe8dc631aULL },   // scr t2r-vox-scrfa-eff-give-03
        { 0xa574d19e9b2d35a8ULL, 0x981b484bb7270aadULL, 0x7903581c2893d53eULL, 0x77f22a1fe8dc5a9bULL },   // scr t2r-vox-scrfa-eff-give-04
        { 0xa574d29e9b2d375bULL, 0x981b474bb72708faULL, 0x7903591c2893d6f1ULL, 0x77f2291fe8dc58e8ULL },   // scr t2r-vox-scrfa-eff-give-05
        { 0xa574d39e9b2d390eULL, 0x981b464bb7270747ULL, 0x7903561c2893d1d8ULL, 0x77f22c1fe8dc5e01ULL },   // scr t2r-vox-scrfa-eff-give-06
        { 0xa574d49e9b2d3ac1ULL, 0x981b454bb7270594ULL, 0x7903571c2893d38bULL, 0x77f22b1fe8dc5c4eULL },   // scr t2r-vox-scrfa-eff-give-07
        { 0xa574cd9e9b2d2edcULL, 0x981b3c4bb726f649ULL, 0x7903541c2893ce72ULL, 0x77f2261fe8dc53cfULL },   // scr t2r-vox-scrfa-eff-give-08
        { 0xa574ce9e9b2d308fULL, 0x981b3b4bb726f496ULL, 0x7903551c2893d025ULL, 0x77f2251fe8dc521cULL },   // scr t2r-vox-scrfa-eff-give-09
        { 0xa577db9e9b2f7c7dULL, 0x9817be4bb723ea58ULL, 0x7906c21c2896bf33ULL, 0x77eea81fe8d947deULL },   // scr t2r-vox-scrfa-eff-give-10
        { 0xa577da9e9b2f7acaULL, 0x9817bf4bb723ec0bULL, 0x7906c11c2896bd80ULL, 0x77eea91fe8d94991ULL },   // scr t2r-vox-scrfa-eff-give-11
        { 0xa577d99e9b2f7917ULL, 0x9817c04bb723edbeULL, 0x7906c41c2896c299ULL, 0x77eea61fe8d94478ULL },   // scr t2r-vox-scrfa-eff-give-12
        { 0xa577d89e9b2f7764ULL, 0x9817c14bb723ef71ULL, 0x7906c31c2896c0e6ULL, 0x77eea71fe8d9462bULL },   // scr t2r-vox-scrfa-eff-give-13
        { 0xa577d79e9b2f75b1ULL, 0x9817c24bb723f124ULL, 0x7906c61c2896c5ffULL, 0x77eeac1fe8d94eaaULL },   // scr t2r-vox-scrfa-eff-give-14
        { 0xa577d69e9b2f73feULL, 0x9817c34bb723f2d7ULL, 0x7906c51c2896c44cULL, 0x77eead1fe8d9505dULL },   // scr t2r-vox-scrfa-eff-give-15
        { 0xa577d59e9b2f724bULL, 0x9817c44bb723f48aULL, 0x7906c81c2896c965ULL, 0x77eeaa1fe8d94b44ULL },   // scr t2r-vox-scrfa-eff-give-16
        { 0xa577d49e9b2f7098ULL, 0x9817c54bb723f63dULL, 0x7906c71c2896c7b2ULL, 0x77eeab1fe8d94cf7ULL },   // scr t2r-vox-scrfa-eff-give-17
        { 0xa577d39e9b2f6ee5ULL, 0x9817b64bb723dcc0ULL, 0x7906ca1c2896cccbULL, 0x77eea01fe8d93a46ULL },   // scr t2r-vox-scrfa-eff-give-18
        { 0xa577d29e9b2f6d32ULL, 0x9817b74bb723de73ULL, 0x7906c91c2896cb18ULL, 0x77eea11fe8d93bf9ULL },   // scr t2r-vox-scrfa-eff-give-19
        { 0xa57b419e9b325fa6ULL, 0x9822304bb72d0093ULL, 0x78fcd01c288e8278ULL, 0x77eb221fe8d62e55ULL },   // scr t2r-vox-scrfa-eff-give-20
        { 0x0c3f8811a3c318c8ULL, 0x72793987ed74a125ULL, 0xf828fd4955fae7f6ULL, 0xcd57b8a687989833ULL },   // mlt t2r-vox-mltfa-eff-give-01
        { 0x0c3f8b11a3c31de1ULL, 0x72793687ed749c0cULL, 0xf828fc4955fae643ULL, 0xcd57b9a6879899e6ULL },   // mlt t2r-vox-mltfa-eff-give-02
        { 0x0c3f8a11a3c31c2eULL, 0x72793787ed749dbfULL, 0xf828fb4955fae490ULL, 0xcd57baa687989b99ULL },   // mlt t2r-vox-mltfa-eff-give-03
        { 0x0c3f8d11a3c32147ULL, 0x72793487ed7498a6ULL, 0xf829024955faf075ULL, 0xcd57bba687989d4cULL },   // mlt t2r-vox-mltfa-eff-give-04
        { 0x0c3f8c11a3c31f94ULL, 0x72793587ed749a59ULL, 0xf829014955faeec2ULL, 0xcd57bca687989effULL },   // mlt t2r-vox-mltfa-eff-give-05
        { 0x0c3f8f11a3c324adULL, 0x72793287ed749540ULL, 0xf829004955faed0fULL, 0xcd57bda68798a0b2ULL },   // mlt t2r-vox-mltfa-eff-give-06
        { 0x0c3f8e11a3c322faULL, 0x72793387ed7496f3ULL, 0xf828ff4955faeb5cULL, 0xcd57bea68798a265ULL },   // mlt t2r-vox-mltfa-eff-give-07
        { 0x0c3f8111a3c30ce3ULL, 0x72794087ed74ad0aULL, 0xf829064955faf741ULL, 0xcd57bfa68798a418ULL },   // mlt t2r-vox-mltfa-eff-give-08
        { 0x0c3f8011a3c30b30ULL, 0x72794187ed74aebdULL, 0xf829054955faf58eULL, 0xcd57c0a68798a5cbULL },   // mlt t2r-vox-mltfa-eff-give-09
        { 0x0c3c0311a3c000f2ULL, 0x727cbe87ed77b8fbULL, 0xf825784955f7d020ULL, 0xcd5b3da6879bb009ULL },   // mlt t2r-vox-mltfa-eff-give-10
        { 0x0c3c0411a3c002a5ULL, 0x727cbd87ed77b748ULL, 0xf825794955f7d1d3ULL, 0xcd5b3ca6879bae56ULL },   // mlt t2r-vox-mltfa-eff-give-11
        { 0x0c3c0111a3bffd8cULL, 0x727cc087ed77bc61ULL, 0xf8257a4955f7d386ULL, 0xcd5b3ba6879baca3ULL },   // mlt t2r-vox-mltfa-eff-give-12
        { 0x0c3c0211a3bfff3fULL, 0x727cbf87ed77baaeULL, 0xf8257b4955f7d539ULL, 0xcd5b3aa6879baaf0ULL },   // mlt t2r-vox-mltfa-eff-give-13
        { 0x0c3bff11a3bffa26ULL, 0x727cc287ed77bfc7ULL, 0xf8257c4955f7d6ecULL, 0xcd5b41a6879bb6d5ULL },   // mlt t2r-vox-mltfa-eff-give-14
        { 0x0c3c0011a3bffbd9ULL, 0x727cc187ed77be14ULL, 0xf8257d4955f7d89fULL, 0xcd5b40a6879bb522ULL },   // mlt t2r-vox-mltfa-eff-give-15
        { 0x0c3bfd11a3bff6c0ULL, 0x727cc487ed77c32dULL, 0xf8257e4955f7da52ULL, 0xcd5b3fa6879bb36fULL },   // mlt t2r-vox-mltfa-eff-give-16
        { 0x0c3bfe11a3bff873ULL, 0x727cc387ed77c17aULL, 0xf8257f4955f7dc05ULL, 0xcd5b3ea6879bb1bcULL },   // mlt t2r-vox-mltfa-eff-give-17
        { 0x0c3c0b11a3c00e8aULL, 0x727cb687ed77ab63ULL, 0xf825804955f7ddb8ULL, 0xcd5b45a6879bbda1ULL },   // mlt t2r-vox-mltfa-eff-give-18
        { 0x0c3c0c11a3c0103dULL, 0x727cb587ed77a9b0ULL, 0xf825814955f7df6bULL, 0xcd5b44a6879bbbeeULL },   // mlt t2r-vox-mltfa-eff-give-19
        { 0x0c38fd11a3bdc0e9ULL, 0x7272ac87ed6f45e0ULL, 0xf82f8a495600433bULL, 0xcd5ec3a6879ec992ULL },   // mlt t2r-vox-mltfa-eff-give-20
        { 0x30f34d57bf6a8044ULL, 0x637dbb0635aebbd5ULL, 0xaeb4281a58f12e56ULL, 0xeace8467a540f9f7ULL },   // scr t2r-vox-scrfa-eff-pain-01
        { 0x30f35057bf6a855dULL, 0x637db80635aeb6bcULL, 0xaeb4271a58f12ca3ULL, 0xeace8567a540fbaaULL },   // scr t2r-vox-scrfa-eff-pain-02
        { 0x30f34f57bf6a83aaULL, 0x637db90635aeb86fULL, 0xaeb4261a58f12af0ULL, 0xeace8667a540fd5dULL },   // scr t2r-vox-scrfa-eff-pain-03
        { 0x30f34a57bf6a7b2bULL, 0x637db60635aeb356ULL, 0xaeb42d1a58f136d5ULL, 0xeace7f67a540f178ULL },   // scr t2r-vox-scrfa-eff-pain-04
        { 0x30f34957bf6a7978ULL, 0x637db70635aeb509ULL, 0xaeb42c1a58f13522ULL, 0xeace8067a540f32bULL },   // scr t2r-vox-scrfa-eff-pain-05
        { 0x30f34c57bf6a7e91ULL, 0x637db40635aeaff0ULL, 0xaeb42b1a58f1336fULL, 0xeace8167a540f4deULL },   // scr t2r-vox-scrfa-eff-pain-06
        { 0x30f34b57bf6a7cdeULL, 0x637db50635aeb1a3ULL, 0xaeb42a1a58f131bcULL, 0xeace8267a540f691ULL },   // scr t2r-vox-scrfa-eff-pain-07
        { 0x30f34657bf6a745fULL, 0x637dc20635aec7baULL, 0xaeb4311a58f13da1ULL, 0xeace7b67a540eaacULL },   // scr t2r-vox-scrfa-eff-pain-08
        { 0x30f34557bf6a72acULL, 0x637dc30635aec96dULL, 0xaeb4301a58f13beeULL, 0xeace7c67a540ec5fULL },   // scr t2r-vox-scrfa-eff-pain-09
        { 0x30efc857bf67686eULL, 0x6381400635b1d3abULL, 0xaeb0a31a58ee1680ULL, 0xead20967a54411cdULL },   // scr t2r-vox-scrfa-eff-pain-10
        { 0x30efc957bf676a21ULL, 0x63813f0635b1d1f8ULL, 0xaeb0a41a58ee1833ULL, 0xead20867a544101aULL },   // scr t2r-vox-scrfa-eff-pain-11
        { 0x30efc657bf676508ULL, 0x6381420635b1d711ULL, 0xaeb0a51a58ee19e6ULL, 0xead20767a5440e67ULL },   // scr t2r-vox-scrfa-eff-pain-12
        { 0x30efc757bf6766bbULL, 0x6381410635b1d55eULL, 0xaeb0a61a58ee1b99ULL, 0xead20667a5440cb4ULL },   // scr t2r-vox-scrfa-eff-pain-13
        { 0x30efcc57bf676f3aULL, 0x6381440635b1da77ULL, 0xaeb0a71a58ee1d4cULL, 0xead20567a5440b01ULL },   // scr t2r-vox-scrfa-eff-pain-14
        { 0x30efcd57bf6770edULL, 0x6381430635b1d8c4ULL, 0xaeb0a81a58ee1effULL, 0xead20467a544094eULL },   // scr t2r-vox-scrfa-eff-pain-15
        { 0x30efca57bf676bd4ULL, 0x6381460635b1ddddULL, 0xaeb0a91a58ee20b2ULL, 0xead20367a544079bULL },   // scr t2r-vox-scrfa-eff-pain-16
        { 0x30efcb57bf676d87ULL, 0x6381450635b1dc2aULL, 0xaeb0aa1a58ee2265ULL, 0xead20267a54405e8ULL },   // scr t2r-vox-scrfa-eff-pain-17
        { 0x30efc057bf675ad6ULL, 0x6381380635b1c613ULL, 0xaeb0ab1a58ee2418ULL, 0xead20167a5440435ULL },   // scr t2r-vox-scrfa-eff-pain-18
        { 0x30efc157bf675c89ULL, 0x6381370635b1c460ULL, 0xaeb0ac1a58ee25cbULL, 0xead20067a5440282ULL },   // scr t2r-vox-scrfa-eff-pain-19
        { 0x30ec4257bf644ee5ULL, 0x63772e0635a96090ULL, 0xaebb351a58f7631bULL, 0xead56f67a546f4f6ULL },   // scr t2r-vox-scrfa-eff-pain-20
        { 0x571847eadc5974fbULL, 0xfb94f99a670915eeULL, 0x4c6e582417d7dc0dULL, 0x924a0924d81df1c0ULL },   // mlt t2r-vox-mltfa-eff-pain-01
        { 0x571848eadc5976aeULL, 0xfb94f89a6709143bULL, 0x4c6e552417d7d6f4ULL, 0x924a0c24d81df6d9ULL },   // mlt t2r-vox-mltfa-eff-pain-02
        { 0x571849eadc597861ULL, 0xfb94f79a67091288ULL, 0x4c6e562417d7d8a7ULL, 0x924a0b24d81df526ULL },   // mlt t2r-vox-mltfa-eff-pain-03
        { 0x57184aeadc597a14ULL, 0xfb94fe9a67091e6dULL, 0x4c6e532417d7d38eULL, 0x924a0e24d81dfa3fULL },   // mlt t2r-vox-mltfa-eff-pain-04
        { 0x57184beadc597bc7ULL, 0xfb94fd9a67091cbaULL, 0x4c6e542417d7d541ULL, 0x924a0d24d81df88cULL },   // mlt t2r-vox-mltfa-eff-pain-05
        { 0x57184ceadc597d7aULL, 0xfb94fc9a67091b07ULL, 0x4c6e512417d7d028ULL, 0x924a1024d81dfda5ULL },   // mlt t2r-vox-mltfa-eff-pain-06
        { 0x57184deadc597f2dULL, 0xfb94fb9a67091954ULL, 0x4c6e522417d7d1dbULL, 0x924a0f24d81dfbf2ULL },   // mlt t2r-vox-mltfa-eff-pain-07
        { 0x57183eeadc5965b0ULL, 0xfb94f29a67090a09ULL, 0x4c6e4f2417d7ccc2ULL, 0x924a1224d81e010bULL },   // mlt t2r-vox-mltfa-eff-pain-08
        { 0x57183feadc596763ULL, 0xfb94f19a67090856ULL, 0x4c6e502417d7ce75ULL, 0x924a1124d81dff58ULL },   // mlt t2r-vox-mltfa-eff-pain-09
        { 0x571bcceadc5c8cd1ULL, 0xfb91749a6705fe18ULL, 0x4c71bd2417dabd83ULL, 0x9246a424d81b104aULL },   // mlt t2r-vox-mltfa-eff-pain-10
        { 0x571bcbeadc5c8b1eULL, 0xfb91759a6705ffcbULL, 0x4c71bc2417dabbd0ULL, 0x9246a524d81b11fdULL },   // mlt t2r-vox-mltfa-eff-pain-11
        { 0x571bcaeadc5c896bULL, 0xfb91769a6706017eULL, 0x4c71bf2417dac0e9ULL, 0x9246a224d81b0ce4ULL },   // mlt t2r-vox-mltfa-eff-pain-12
        { 0x571bc9eadc5c87b8ULL, 0xfb91779a67060331ULL, 0x4c71be2417dabf36ULL, 0x9246a324d81b0e97ULL },   // mlt t2r-vox-mltfa-eff-pain-13
        { 0x571bd0eadc5c939dULL, 0xfb91789a670604e4ULL, 0x4c71c12417dac44fULL, 0x9246a024d81b097eULL },   // mlt t2r-vox-mltfa-eff-pain-14
        { 0x571bcfeadc5c91eaULL, 0xfb91799a67060697ULL, 0x4c71c02417dac29cULL, 0x9246a124d81b0b31ULL },   // mlt t2r-vox-mltfa-eff-pain-15
        { 0x571bceeadc5c9037ULL, 0xfb917a9a6706084aULL, 0x4c71c32417dac7b5ULL, 0x92469e24d81b0618ULL },   // mlt t2r-vox-mltfa-eff-pain-16
        { 0x571bcdeadc5c8e84ULL, 0xfb917b9a670609fdULL, 0x4c71c22417dac602ULL, 0x92469f24d81b07cbULL },   // mlt t2r-vox-mltfa-eff-pain-17
        { 0x571bc4eadc5c7f39ULL, 0xfb916c9a6705f080ULL, 0x4c71c52417dacb1bULL, 0x92469c24d81b02b2ULL },   // mlt t2r-vox-mltfa-eff-pain-18
        { 0x571bc3eadc5c7d86ULL, 0xfb916d9a6705f233ULL, 0x4c71c42417dac968ULL, 0x92469d24d81b0465ULL },   // mlt t2r-vox-mltfa-eff-pain-19
        { 0xa38072e037f7bfeaULL, 0xa1fe932dbc0f728dULL, 0x1731b68e94409fb4ULL, 0x0edd021c549c04c7ULL },   // scr t2r-vox-scrfa-eff-death-01
        { 0xa38071e037f7be37ULL, 0xa1fe902dbc0f6d74ULL, 0x1731b98e9440a4cdULL, 0x0edd031c549c067aULL },   // scr t2r-vox-scrfa-eff-death-02
        { 0xa38070e037f7bc84ULL, 0xa1fe912dbc0f6f27ULL, 0x1731b88e9440a31aULL, 0x0edd041c549c082dULL },   // scr t2r-vox-scrfa-eff-death-03
        { 0xa3806fe037f7bad1ULL, 0xa1fe8e2dbc0f6a0eULL, 0x1731b38e94409a9bULL, 0x0edcfd1c549bfc48ULL },   // scr t2r-vox-scrfa-eff-death-04
        { 0xa3806ee037f7b91eULL, 0xa1fe8f2dbc0f6bc1ULL, 0x1731b28e944098e8ULL, 0x0edcfe1c549bfdfbULL },   // scr t2r-vox-scrfa-eff-death-05
        { 0xa3806de037f7b76bULL, 0xa1fe8c2dbc0f66a8ULL, 0x1731b58e94409e01ULL, 0x0edcff1c549bffaeULL },   // scr t2r-vox-scrfa-eff-death-06
        { 0xa3806ce037f7b5b8ULL, 0xa1fe8d2dbc0f685bULL, 0x1731b48e94409c4eULL, 0x0edd001c549c0161ULL },   // scr t2r-vox-scrfa-eff-death-07
        { 0xa3806be037f7b405ULL, 0xa1fe8a2dbc0f6342ULL, 0x1731af8e944093cfULL, 0x0edcf91c549bf57cULL },   // scr t2r-vox-scrfa-eff-death-08
        { 0xa3806ae037f7b252ULL, 0xa1fe8b2dbc0f64f5ULL, 0x1731ae8e9440921cULL, 0x0edcfa1c549bf72fULL },   // scr t2r-vox-scrfa-eff-death-09
        { 0xa37cede037f4a814ULL, 0xa201f82dbc125403ULL, 0x172e318e943d87deULL, 0x0ee0871c549f1c9dULL },   // scr t2r-vox-scrfa-eff-death-10
        { 0xa37ceee037f4a9c7ULL, 0xa201f72dbc125250ULL, 0x172e328e943d8991ULL, 0x0ee0861c549f1aeaULL },   // scr t2r-vox-scrfa-eff-death-11
        { 0xa37cefe037f4ab7aULL, 0xa201fa2dbc125769ULL, 0x172e2f8e943d8478ULL, 0x0ee0851c549f1937ULL },   // scr t2r-vox-scrfa-eff-death-12
        { 0xa37cf0e037f4ad2dULL, 0xa201f92dbc1255b6ULL, 0x172e308e943d862bULL, 0x0ee0841c549f1784ULL },   // scr t2r-vox-scrfa-eff-death-13
        { 0xa37ce9e037f4a148ULL, 0xa201fc2dbc125acfULL, 0x172e358e943d8eaaULL, 0x0ee0831c549f15d1ULL },   // scr t2r-vox-scrfa-eff-death-14
        { 0x37fad770ce671533ULL, 0xadb6784d2241b72cULL, 0x18bc8dc058300d85ULL, 0xe53cada91063cbaeULL },   // mlt t2r-vox-mltfa-eff-death-01
        { 0x37fad870ce6716e6ULL, 0xadb67b4d2241bc45ULL, 0x18bc8ac05830086cULL, 0xe53caca91063c9fbULL },   // mlt t2r-vox-mltfa-eff-death-02
        { 0x37fad970ce671899ULL, 0xadb67a4d2241ba92ULL, 0x18bc8bc058300a1fULL, 0xe53caba91063c848ULL },   // mlt t2r-vox-mltfa-eff-death-03
        { 0x37fada70ce671a4cULL, 0xadb6754d2241b213ULL, 0x18bc88c058300506ULL, 0xe53cb2a91063d42dULL },   // mlt t2r-vox-mltfa-eff-death-04
        { 0x37fadb70ce671bffULL, 0xadb6744d2241b060ULL, 0x18bc89c0583006b9ULL, 0xe53cb1a91063d27aULL },   // mlt t2r-vox-mltfa-eff-death-05
        { 0x37fadc70ce671db2ULL, 0xadb6774d2241b579ULL, 0x18bc86c0583001a0ULL, 0xe53cb0a91063d0c7ULL },   // mlt t2r-vox-mltfa-eff-death-06
        { 0x37fadd70ce671f65ULL, 0xadb6764d2241b3c6ULL, 0x18bc87c058300353ULL, 0xe53cafa91063cf14ULL },   // mlt t2r-vox-mltfa-eff-death-07
        { 0x37fade70ce672118ULL, 0xadb6814d2241c677ULL, 0x18bc94c05830196aULL, 0xe53ca6a91063bfc9ULL },   // mlt t2r-vox-mltfa-eff-death-08
        { 0x37fadf70ce6722cbULL, 0xadb6804d2241c4c4ULL, 0x18bc95c058301b1dULL, 0xe53ca5a91063be16ULL },   // mlt t2r-vox-mltfa-eff-death-09
        { 0x37fe5c70ce6a2d09ULL, 0xadb2f34d223e9f56ULL, 0x18c012c05833255bULL, 0xe53928a91060b3d8ULL },   // mlt t2r-vox-mltfa-eff-death-10
        { 0x37fe5b70ce6a2b56ULL, 0xadb2f44d223ea109ULL, 0x18c011c0583323a8ULL, 0xe53929a91060b58bULL },   // mlt t2r-vox-mltfa-eff-death-11
        { 0x37fe5a70ce6a29a3ULL, 0xadb2f14d223e9bf0ULL, 0x18c014c0583328c1ULL, 0xe5392aa91060b73eULL },   // mlt t2r-vox-mltfa-eff-death-12
        { 0x37fe5970ce6a27f0ULL, 0xadb2f24d223e9da3ULL, 0x18c013c05833270eULL, 0xe5392ba91060b8f1ULL },   // mlt t2r-vox-mltfa-eff-death-13
        { 0x37fe6070ce6a33d5ULL, 0xadb2f74d223ea622ULL, 0x18c016c058332c27ULL, 0xe5392ca91060baa4ULL },   // mlt t2r-vox-mltfa-eff-death-14
        { 0x37fe5f70ce6a3222ULL, 0xadb2f84d223ea7d5ULL, 0x18c015c058332a74ULL, 0xe5392da91060bc57ULL },   // mlt t2r-vox-mltfa-eff-death-15
        { 0x37fe5e70ce6a306fULL, 0xadb2f54d223ea2bcULL, 0x18c018c058332f8dULL, 0xe5392ea91060be0aULL },   // mlt t2r-vox-mltfa-eff-death-16
        { 0x37fe5d70ce6a2ebcULL, 0xadb2f64d223ea46fULL, 0x18c017c058332ddaULL, 0xe5392fa91060bfbdULL },   // mlt t2r-vox-mltfa-eff-death-17
        { 0x279f8c5ee4dbfb8dULL, 0x04d1ce4dfb7dd3ccULL, 0xe396444139915ecfULL, 0x9fecc575fd0b2196ULL },   // scr t2r-vox-scrfa-eff-deathknife-01
        { 0x279f895ee4dbf674ULL, 0x04d1d14dfb7dd8e5ULL, 0xe396454139916082ULL, 0x9fecc475fd0b1fe3ULL },   // scr t2r-vox-scrfa-eff-deathknife-02
        { 0x279f8a5ee4dbf827ULL, 0x04d1d04dfb7dd732ULL, 0xe396464139916235ULL, 0x9fecc375fd0b1e30ULL },   // scr t2r-vox-scrfa-eff-deathknife-03
        { 0x279f875ee4dbf30eULL, 0x04d1cb4dfb7dceb3ULL, 0xe3963f4139915650ULL, 0x9fecca75fd0b2a15ULL },   // scr t2r-vox-scrfa-eff-deathknife-04
        { 0x279f885ee4dbf4c1ULL, 0x04d1ca4dfb7dcd00ULL, 0xe396404139915803ULL, 0x9fecc975fd0b2862ULL },   // scr t2r-vox-scrfa-eff-deathknife-05
        { 0x279f855ee4dbefa8ULL, 0x04d1cd4dfb7dd219ULL, 0xe3964141399159b6ULL, 0x9fecc875fd0b26afULL },   // scr t2r-vox-scrfa-eff-deathknife-06
        { 0x279f865ee4dbf15bULL, 0x04d1cc4dfb7dd066ULL, 0xe396424139915b69ULL, 0x9fecc775fd0b24fcULL },   // scr t2r-vox-scrfa-eff-deathknife-07
        { 0x279f835ee4dbec42ULL, 0x04d1d74dfb7de317ULL, 0xe3964b4139916ab4ULL, 0x9fecce75fd0b30e1ULL },   // scr t2r-vox-scrfa-eff-deathknife-08
        { 0x279f845ee4dbedf5ULL, 0x04d1d64dfb7de164ULL, 0xe3964c4139916c67ULL, 0x9feccd75fd0b2f2eULL },   // scr t2r-vox-scrfa-eff-deathknife-09
        { 0x27a2f15ee4dedd03ULL, 0x04cec94dfb7b9576ULL, 0xe399494139939d25ULL, 0x9fe94075fd0809c0ULL },   // scr t2r-vox-scrfa-eff-deathknife-10
        { 0x27a2f05ee4dedb50ULL, 0x04ceca4dfb7b9729ULL, 0xe399484139939b72ULL, 0x9fe94175fd080b73ULL },   // scr t2r-vox-scrfa-eff-deathknife-11
        { 0x27a2f35ee4dee069ULL, 0x04cec74dfb7b9210ULL, 0xe3994741399399bfULL, 0x9fe94275fd080d26ULL },   // scr t2r-vox-scrfa-eff-deathknife-12
        { 0x27a2f25ee4dedeb6ULL, 0x04cec84dfb7b93c3ULL, 0xe39946413993980cULL, 0x9fe94375fd080ed9ULL },   // scr t2r-vox-scrfa-eff-deathknife-13
        { 0x27a2f55ee4dee3cfULL, 0x04cecd4dfb7b9c42ULL, 0xe399454139939659ULL, 0x9fe94475fd08108cULL },   // scr t2r-vox-scrfa-eff-deathknife-14
        { 0xd97527fd6903d2f2ULL, 0xdf6bff025c4954a7ULL, 0xf38d7f2419670564ULL, 0xe53c4d1e2aa9c671ULL },   // mlt t2r-vox-mltfa-eff-deathknife-01
        { 0xd97526fd6903d13fULL, 0xdf6c00025c49565aULL, 0xf38d822419670a7dULL, 0xe53c4a1e2aa9c158ULL },   // mlt t2r-vox-mltfa-eff-deathknife-02
        { 0xd97525fd6903cf8cULL, 0xdf6c01025c49580dULL, 0xf38d8124196708caULL, 0xe53c4b1e2aa9c30bULL },   // mlt t2r-vox-mltfa-eff-deathknife-03
        { 0xd97524fd6903cdd9ULL, 0xdf6bfa025c494c28ULL, 0xf38d7c241967004bULL, 0xe53c501e2aa9cb8aULL },   // mlt t2r-vox-mltfa-eff-deathknife-04
        { 0xd97523fd6903cc26ULL, 0xdf6bfb025c494ddbULL, 0xf38d7b241966fe98ULL, 0xe53c511e2aa9cd3dULL },   // mlt t2r-vox-mltfa-eff-deathknife-05
        { 0xd97522fd6903ca73ULL, 0xdf6bfc025c494f8eULL, 0xf38d7e24196703b1ULL, 0xe53c4e1e2aa9c824ULL },   // mlt t2r-vox-mltfa-eff-deathknife-06
        { 0xd97521fd6903c8c0ULL, 0xdf6bfd025c495141ULL, 0xf38d7d24196701feULL, 0xe53c4f1e2aa9c9d7ULL },   // mlt t2r-vox-mltfa-eff-deathknife-07
        { 0xd97530fd6903e23dULL, 0xdf6bf6025c49455cULL, 0xf38d78241966f97fULL, 0xe53c441e2aa9b726ULL },   // mlt t2r-vox-mltfa-eff-deathknife-08
        { 0xd9752ffd6903e08aULL, 0xdf6bf7025c49470fULL, 0xf38d77241966f7ccULL, 0xe53c451e2aa9b8d9ULL },   // mlt t2r-vox-mltfa-eff-deathknife-09
        { 0xd97222fd6901949cULL, 0xdf6f04025c4b92fdULL, 0xf38a7a241964c70eULL, 0xe53fd21e2aacde47ULL },   // mlt t2r-vox-mltfa-eff-deathknife-10
        { 0xd97223fd6901964fULL, 0xdf6f03025c4b914aULL, 0xf38a7b241964c8c1ULL, 0xe53fd11e2aacdc94ULL },   // mlt t2r-vox-mltfa-eff-deathknife-11
    };

    inline constexpr uint16_t kRange[core::kFactionCount * core::kKindCount][2] = {
        {0, 20}, {40, 20}, {79, 14}, {110, 14},   // scr
        {20, 20}, {60, 19}, {93, 17}, {124, 11},   // mlt
    };

    inline constexpr bool Have = true;

    inline core::View View() noexcept
    {
        return { kLines, static_cast<int>(sizeof kLines / sizeof kLines[0]), kRange };
    }

#else

    inline constexpr bool Have = false;

    inline core::View View() noexcept { return {}; }

#endif

} // namespace net::audio::effort::data
