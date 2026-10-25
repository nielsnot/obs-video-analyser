'''
Check the HDR HLG signal path.

A white color source fills the picture. A luma waveform in a dock shows where white lands:
- SDR (Rec. 709): at 100 %.
- HLG: SDR white (OBS default 300 nits) encoded like OBS's own HLG output, about 81 %.

The second test switches the OBS project itself to Rec. 2100 HLG, so the picture is captured
from OBS's HDR canvas in linear float.
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
WV_SIZE = 256
# HLG signal of SDR white at 300 nits on a 1000 nits display
HLG_WHITE = 0.813


def notice(msg):
    'Show a line in the GitHub Actions summary.'
    print(f'::notice::{msg}', flush=True)


def dock_data(colorspace):
    'A dock with a luma waveform following the selection.'
    scopes = [{
        'type': 'waveform',
        'label': 'WV', 'show_label': False, 'use_roi': True, 'target': '',
        'prop': {'components': 0x20, 'display': 0, 'graticule_lines': 0, 'scale_labels': 0,
                 'intensity': 255, 'target_scale': 2},
    }]
    return {
        'docks': [{
            'name': 'HLG test',
            'toolbar': True,
            'active_view': 0,
            'roi-prop': {'target_name': '', 'interleave': 0, 'target_scale': 2, 'shapes': [],
                         'colorspace': colorspace},
            'views': [{'name': 'View 1', 'state': {'layout': 0, 'roi-shown': True, 'scopes': scopes}}],
        }],
    }


class HLGTestBase(obstest.OBSTest):
    'Common setup: write the dock, optionally switch the project to HLG, start OBS'
    colorspace = 0
    hdr_project = False

    def setUp(self, config_name='saved-config', run=True):
        super().setUp(run=False, config_name=config_name)
        path = self.obs.config.get_scenecollection_file()
        if not os.path.exists(path) and os.path.exists(path + '.json'):
            path += '.json'
        with open(path, encoding='utf-8') as fr:
            collection = json.load(fr)
        collection.setdefault('modules', {})[DOCK_KEY] = dock_data(self.colorspace)
        with open(path, 'w', encoding='utf-8') as fw:
            json.dump(collection, fw)
        if self.hdr_project:
            profile = self.obs.config.get_profile()
            profile['AdvVideo']['ColorSpace'] = '2100HLG'
            profile['AdvVideo']['ColorFormat'] = 'P010'
            profile['AdvVideo']['ColorRange'] = 'Partial'
            profile.save()
        self.obs.run()

    def screenshot(self, cl, name, repeat=40):
        'Render the source repeatedly so its pipeline has data, then return the last image.'
        from PIL import Image  # pylint: disable=import-outside-toplevel
        data = None
        for _ in range(repeat):
            data = cl.send('GetSourceScreenshot', {'sourceName': name, 'imageFormat': 'png'}).image_data
            time.sleep(0.05)
        raw = base64.b64decode(data.split(',', 1)[1])
        img = Image.open(io.BytesIO(raw)).convert('RGB')
        os.makedirs('screenshots', exist_ok=True)
        img.save(f'screenshots/{self.name}-{name}-{int(time.time() * 1000)}.png')
        return img

    @staticmethod
    def trace_level(img):
        'Level (0..1) of the brightest row of the waveform.'
        best_row, best = 0, -1
        for y in range(img.height):
            total = sum(sum(img.getpixel((x, y))) for x in range(0, img.width, 4))
            if total > best:
                best, best_row = total, y
        return 1.0 - (best_row + 0.5) / img.height, best

    def white_level(self):
        'Fill the picture with white and return where the luma waveform shows it.'
        cl = self.obs.get_obsws()
        video = cl.send('GetVideoSettings')
        cl.send('CreateInput', {
            'sceneName': 'Scene', 'inputName': 'White', 'inputKind': 'color_source_v3',
            'inputSettings': {'color': 0xFFFFFFFF, 'width': video.base_width, 'height': video.base_height},
        })
        names = [i['inputName'] for i in cl.send('GetInputList').inputs
                 if i['inputKind'].startswith(PREFIX + 'waveform_source')]
        self.assertTrue(names, 'the dock waveform was not created')
        img = self.screenshot(cl, names[0])
        self.assertEqual(img.height, WV_SIZE)
        level, strength = self.trace_level(img)
        return level, strength


class SDRProjectTest(HLGTestBase):
    'SDR project: Rec. 709 versus HLG chosen for the dock'

    def test_sdr_and_forced_hlg(self):
        'White at 100 % in Rec. 709, at the HLG level of SDR white when HLG is chosen'
        level, strength = self.white_level()
        notice(f'SDR project, Auto: white at {level:.3f} (strength {strength})')
        self.assertGreater(level, 0.97, 'white is not at 100 % in Rec. 709')

        cl = self.obs.get_obsws()
        roi = [i['inputName'] for i in cl.send('GetInputList').inputs
               if i['inputKind'].startswith(PREFIX + 'roi')][0]
        cl.send('SetInputSettings', {'inputName': roi, 'inputSettings': {'colorspace': 3}})
        names = [i['inputName'] for i in cl.send('GetInputList').inputs
                 if i['inputKind'].startswith(PREFIX + 'waveform_source')]
        img = self.screenshot(cl, names[0])
        level, strength = self.trace_level(img)
        notice(f'SDR project, HLG chosen: white at {level:.3f} (strength {strength})')
        self.assertAlmostEqual(level, HLG_WHITE, delta=0.03, msg='white is not at the HLG level of SDR white')


class HLGProjectTest(HLGTestBase):
    'OBS project in Rec. 2100 HLG: the dock follows it automatically'
    hdr_project = True

    def test_hlg_project(self):
        'White from the HDR canvas lands at the HLG level of SDR white'
        level, strength = self.white_level()
        notice(f'HLG project, Auto: white at {level:.3f} (strength {strength})')
        self.assertAlmostEqual(level, HLG_WHITE, delta=0.03, msg='white is not at the HLG level of SDR white')


if __name__ == '__main__':
    unittest.main()
