project('bt-phone-recorder', 'c',
  version : '0.1.0',
  license : 'BSD-3-Clause',
  meson_version : '>= 1.3.0',
  default_options : ['c_std=gnu17', 'warning_level=2', 'buildtype=debugoptimized',
                     'b_ndebug=if-release'])

cc = meson.get_compiler('c')
i18n = import('i18n')
gnome = import('gnome')

app_id = 'guru.freberg.BtPhoneRecorder'
prefix = get_option('prefix')
libexecdir = prefix / get_option('libexecdir')
localedir = prefix / get_option('localedir')

# Minimum versions follow the current releases this was developed against.
gtk_dep = dependency('gtk4', version : '>= 4.18')
adw_dep = dependency('libadwaita-1', version : '>= 1.7')
pw_dep = dependency('libpipewire-0.3', version : '>= 1.4')
usb_dep = dependency('libusb-1.0', version : '>= 1.0.27')
sndfile_dep = dependency('sndfile', version : '>= 1.2')
speex_dep = dependency('speexdsp', version : '>= 1.2.1')
zstd_dep = dependency('libzstd', version : '>= 1.5')
lzma_dep = dependency('liblzma', version : '>= 5.4')
soup_dep = dependency('libsoup-3.0', version : '>= 3.6', required : get_option('obs_websocket'))
json_dep = dependency('json-glib-1.0', version : '>= 1.10', required : get_option('obs_websocket'))
m_dep = cc.find_library('m')
thread_dep = dependency('threads')

# ---------------------------------------------------------------------------
# BTstack (git submodule, pinned to the latest release)
# ---------------------------------------------------------------------------
bt = 'third_party/btstack'
btstack_inc = include_directories(
  'src/bt',
  bt / 'src', bt / 'src/classic', bt / 'src/ble',
  bt / 'platform/posix', bt / 'platform/embedded',
  bt / 'chipset/realtek', bt / 'chipset/intel', bt / 'chipset/bcm',
  bt / '3rd-party/bluedroid/decoder/include', bt / '3rd-party/bluedroid/encoder/include',
  bt / '3rd-party/lc3-google/include', bt / '3rd-party/tinydir',
  bt / '3rd-party/micro-ecc', bt / '3rd-party/rijndael', bt / '3rd-party/yxml', bt / '3rd-party/md5',
)

btstack_src = files(
  bt / 'src/btstack_memory.c', bt / 'src/btstack_linked_list.c',
  bt / 'src/btstack_memory_pool.c', bt / 'src/btstack_run_loop.c',
  bt / 'src/btstack_run_loop_base.c', bt / 'src/btstack_util.c',
  bt / 'src/hci.c', bt / 'src/hci_cmd.c', bt / 'src/hci_dump.c', bt / 'src/hci_event.c',
  bt / 'src/l2cap.c', bt / 'src/l2cap_signaling.c', bt / 'src/btstack_audio.c',
  bt / 'src/btstack_tlv.c', bt / 'src/btstack_crypto.c', bt / 'src/btstack_ring_buffer.c',
  bt / 'src/btstack_lc3.c', bt / 'src/btstack_lc3_google.c', bt / 'src/ad_parser.c',
  bt / 'src/btstack_ltv_builder.c',
  bt / 'src/hci_transport_h4.c', bt / 'src/hci_transport_h5.c', bt / 'src/btstack_slip.c',
  bt / 'src/btstack_uart_slip_wrapper.c',
  bt / 'src/classic/sdp_util.c', bt / 'src/classic/sdp_server.c',
  bt / 'src/classic/sdp_client.c', bt / 'src/classic/sdp_client_rfcomm.c',
  bt / 'src/classic/rfcomm.c', bt / 'src/classic/hfp.c', bt / 'src/classic/hfp_hf.c',
  bt / 'src/classic/btstack_link_key_db_tlv.c', bt / 'src/classic/btstack_cvsd_plc.c',
  bt / 'src/classic/btstack_sbc_plc.c', bt / 'src/classic/btstack_sbc_bluedroid.c',
  bt / 'src/classic/btstack_sbc_decoder_bluedroid.c',
  bt / 'src/classic/btstack_sbc_encoder_bluedroid.c',
  bt / 'src/classic/hfp_msbc.c', bt / 'src/classic/hfp_codec.c',
  bt / 'src/classic/goep_client.c', bt / 'src/classic/obex_iterator.c',
  bt / 'src/classic/obex_parser.c', bt / 'src/classic/obex_message_builder.c',
  bt / 'src/classic/obex_srm_client.c', bt / 'src/classic/pbap_client.c',
  bt / '3rd-party/md5/md5.c', bt / '3rd-party/yxml/yxml.c',
  bt / 'platform/posix/btstack_run_loop_posix.c', bt / 'platform/posix/btstack_tlv_posix.c',
  bt / 'platform/posix/hci_dump_posix_fs.c', bt / 'platform/posix/btstack_uart_posix.c',
  bt / 'platform/libusb/hci_transport_h2_libusb.c',
  bt / 'chipset/intel/btstack_chipset_intel_firmware.c',
  bt / 'chipset/bcm/btstack_chipset_bcm.c',
)
foreach f : ['alloc', 'bitalloc', 'bitalloc-sbc', 'bitstream-decode', 'decoder-oina',
             'decoder-private', 'decoder-sbc', 'dequant', 'framing', 'framing-sbc',
             'oi_codec_version', 'synthesis-sbc', 'synthesis-dct8', 'synthesis-8-generated']
  btstack_src += files(bt / '3rd-party/bluedroid/decoder/srce' / f + '.c')
endforeach
foreach f : ['sbc_analysis', 'sbc_dct', 'sbc_dct_coeffs', 'sbc_enc_bit_alloc_mono',
             'sbc_enc_bit_alloc_ste', 'sbc_enc_coeffs', 'sbc_encoder', 'sbc_packing']
  btstack_src += files(bt / '3rd-party/bluedroid/encoder/srce' / f + '.c')
endforeach
foreach f : ['attdet', 'bits', 'bwdet', 'energy', 'lc3', 'ltpf', 'mdct', 'plc', 'sns', 'spec',
             'tables', 'tns']
  btstack_src += files(bt / '3rd-party/lc3-google/src' / f + '.c')
endforeach

# Small fixes to BTstack live in patches/ and are applied to copies in the
# build folder, so the submodule stays pristine and easy to update. A patch
# that no longer applies stops the build.
patch_prog = find_program('patch')
btstack_src += custom_target('btstack_chipset_realtek_patched',
  input : [bt / 'chipset/realtek/btstack_chipset_realtek.c',
           'patches/0001-realtek-ignore-stale-card-info-reply.patch'],
  output : 'btstack_chipset_realtek.c',
  command : [patch_prog, '--quiet', '--forward', '-o', '@OUTPUT@', '@INPUT0@', '@INPUT1@'],
)

# Third-party code: build it as upstream intends, without our warning level.
btstack_lib = static_library('btstack', btstack_src,
  include_directories : btstack_inc,
  dependencies : [usb_dep, m_dep, thread_dep],
  c_args : ['-w', '-DHAVE_POSIX_FILE_IO'],
  override_options : ['warning_level=0'],
)
btstack_dep = declare_dependency(link_with : btstack_lib, include_directories : btstack_inc,
                                 dependencies : [usb_dep, m_dep, thread_dep])

# ---------------------------------------------------------------------------
# Core library (no GUI): audio, recording, Bluetooth. Shared by app and tests.
# ---------------------------------------------------------------------------
core_args = [
  '-DBPR_LIBEXECDIR="' + libexecdir + '"',
  '-Wshadow', '-Wstrict-prototypes', '-Wmissing-prototypes', '-Wno-unused-parameter',
  '-Wno-format-truncation',
]

core_src = files(
  'src/core/ringbuf.c', 'src/core/log.c', 'src/core/paths.c', 'src/core/vcard.c',
  'src/audio/adaptive.c', 'src/audio/dsp.c', 'src/audio/mixer.c', 'src/audio/phone_link.c',
  'src/audio/engine.c',
  'src/rec/recorder.c',
  'src/bt/adapters.c', 'src/bt/firmware.c', 'src/bt/usb_guard.c', 'src/bt/hci_user_channel.c',
  'src/bt/bt_engine.c',
)
core_lib = static_library('bprcore', core_src,
  c_args : core_args,
  dependencies : [btstack_dep, pw_dep, sndfile_dep, speex_dep, zstd_dep, lzma_dep, m_dep,
                  thread_dep],
)
core_dep = declare_dependency(link_with : core_lib,
  include_directories : include_directories('src'),
  dependencies : [btstack_dep, pw_dep, sndfile_dep, speex_dep, zstd_dep, lzma_dep, m_dep,
                  thread_dep])

subdir('po')
subdir('data')
subdir('src/ui')
subdir('tools')
subdir('tests')

summary({
  'OBS websocket control' : soup_dep.found() and json_dep.found(),
  'BTstack' : 'submodule ' + bt,
}, section : 'Features')
