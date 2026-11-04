'''
Raw input: a 10-bit clip with superwhite (Y 1000 of 1023, about 107 %) on the left and sub-black (Y 20, about -5 %)
on the right. A waveform on that media source gets the raw signal from the filter the plugin adds by itself, so the
trace sits above the 100 % line and below the 0 % line instead of being clipped at the edges.
'''

import base64
import io
import json
import os
import time
import unittest
from onsdriver import obstest

DOCK_KEY = 'obs-video-analyser-dock'
PREFIX = 'obs-video-analyser.'
ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets')
CLIP = os.path.join(ASSETS, 'superwhite.mkv')
CLIP_HLG = os.path.join(ASSETS, 'superwhite-hlg.mkv')  # the same codes, tagged Rec. 2100 HLG
SOURCE = 'Raw clip'


def notice(msg):
    'Show a line in the GitHub Actions summary.'
    print(f'::notice::{msg}', flush=True)


def dock_data():
    'A dock on the clip with a luma waveform, without graticule, labels or min/max lines.'
    scopes = [{'type': 'waveform', 'label': 'Waveform', 'show_label': False, 'use_roi': True, 'target': '',
               'prop': {'target_scale': 2, 'components': 0x20, 'display': 0, 'intensity': 255,
                        'graticule_lines': 0, 'scale_labels': 0, 'minmax': False, 'out_highlight': False, 'hdr_ref': False}}]
    return {'docks': [{
        'name': 'Raw', 'toolbar': True, 'active_view': 0,
        'roi-prop': {'target_name': SOURCE, 'interleave': 0, 'target_scale': 2, 'shapes': []},
        'views': [{'name': 'View 1', 'state': {'layout': 0, 'roi-shown': True, 'scopes': scopes}}],
    }]}


class RawInputTest(obstest.OBSTest):
    'The raw signal of a video source'
    clip = CLIP
    hdr_project = False
    expect_raw = True

    def setUp(self, config_name='saved-config', run=True):
        super().setUp(run=False, config_name=config_name)
        path = self.obs.config.get_scenecollection_file()
        if not os.path.exists(path) and os.path.exists(path + '.json'):
            path += '.json'
        with open(path, encoding='utf-8') as fr:
            collection = json.load(fr)
        collection.setdefault('modules', {})[DOCK_KEY] = dock_data()
        with open(path, 'w', encoding='utf-8') as fw:
            json.dump(collection, fw)
        if self.hdr_project:
            profile = self.obs.config.get_profile()
            video = profile['Video']
            video['ColorSpace'] = '2100HLG'
            video['ColorFormat'] = 'P010'
            video['ColorRange'] = 'Partial'
            profile.save()
        self.obs.run()

    def test_raw(self):
        'The trace goes beyond 100 % and below 0 % when the source has the transfer of the project'
        from PIL import Image  # pylint: disable=import-outside-toplevel
        cl = self.obs.get_obsws()
        try:
            cl.send('CreateInput', {
                'sceneName': 'Scene', 'inputName': SOURCE, 'inputKind': 'ffmpeg_source',
                'inputSettings': {'is_local_file': True, 'local_file': self.clip, 'looping': True,
                                  'restart_on_activate': True},
            })
        except Exception as e:  # pylint: disable=broad-except
            print(f'::warning::raw input: no media source ({e})', flush=True)
            self.skipTest('no media source')
        name = [i['inputName'] for i in cl.send('GetInputList').inputs
                if i['inputKind'].startswith(PREFIX + 'waveform_source')][0]
        rows = []
        img = None
        filters = []
        for _ in range(100):
            time.sleep(0.15)
            try:
                filters = [f['filterKind'] for f in cl.send('GetSourceFilterList', {'sourceName': SOURCE}).filters]
                data = cl.send('GetSourceScreenshot', {'sourceName': name, 'imageFormat': 'png',
                                                       'imageWidth': 320, 'imageHeight': 256}).image_data
            except Exception:  # pylint: disable=broad-except
                continue
            img = Image.open(io.BytesIO(base64.b64decode(data.split(',', 1)[1]))).convert('RGB')
            rows = [y for y in range(img.height)
                    if any(sum(img.getpixel((x, y))) > 90 for x in range(0, img.width, 4))]
            # 107 % at code 250 of 255 (row 5), -5 % at code 5 (row 250); clipped would be rows 0 and 255
            raw = rows and 2 <= rows[0] <= 10 and 245 <= rows[-1] <= 253
            if raw and self.expect_raw:
                break
            if rows and not self.expect_raw and not raw and filters:
                break
        if img:
            os.makedirs('screenshots', exist_ok=True)
            img.save('screenshots/raw-input.png')
        notice(f'raw input ({type(self).__name__}): filters {filters}; waveform rows {rows[:3]}..{rows[-3:]}')
        self.assertIn(PREFIX + 'raw_input', filters, 'the raw input filter was not added')
        self.assertTrue(rows, 'the waveform draws nothing')
        if self.expect_raw:
            self.assertTrue(2 <= rows[0] <= 10, f'superwhite not above 100 %: top row {rows[0]}')
            self.assertTrue(245 <= rows[-1] <= 253, f'sub-black not below 0 %: bottom row {rows[-1]}')
        else:
            # converted by OBS, not the raw signal: superwhite does not sit at 107 % (row 5), sub-black not at -5 %
            self.assertFalse(2 <= rows[0] <= 10 and 245 <= rows[-1] <= 253,
                             f'an SDR source in an HLG project is shown raw: rows {rows[0]}..{rows[-1]}')


class RawInputHLGTest(RawInputTest):
    'An HLG source in an HLG project: the raw signal is the HLG signal itself'
    clip = CLIP_HLG
    hdr_project = True


class RawInputSDRinHLGTest(RawInputTest):
    'An SDR source in an HLG project: OBS converts it, so the scopes measure the converted picture'
    clip = CLIP
    hdr_project = True
    expect_raw = False


if __name__ == '__main__':
    unittest.main()
