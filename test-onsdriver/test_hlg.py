'''
Check the HDR HLG signal path.

A white color source fills the picture. False color shows where white lands:
- SDR (Rec. 709): at 100 %, the "clipped" band (red).
- HLG: SDR white (OBS default 300 nits) encoded like OBS's own HLG output is about 81 %,
  the "highlights" band (dark yellow) of the Exposure preset.

The second test switches the OBS project itself to Rec. 2100 HLG, so the picture is captured
from OBS's HDR canvas in linear float.
'''

import base64
import io
import json
import os
import re
import time
import unittest
from onsdriver import obstest

DOCK_KEY = 'obs-video-analyser-dock'
PREFIX = 'obs-video-analyser.'

CLIP = (230, 51, 0)         # Exposure preset, 100 % and above
HIGHLIGHT = (179, 179, 0)   # Exposure preset, 78-84 %


def notice(msg):
    'Show a line in the GitHub Actions summary.'
    print(f'::notice::{msg}', flush=True)


def dock_data(colorspace):
    'A dock with a false color scope and a luma waveform following the selection.'
    scopes = [
        {
            'type': 'falsecolor',
            'label': 'FC', 'show_label': False, 'use_roi': True, 'target': '',
            'prop': {'fc_preset': 0, 'show_key': 0, 'target_scale': 2},
        },
        {
            'type': 'waveform',
            'label': 'WV', 'show_label': False, 'use_roi': True, 'target': '',
            'prop': {'components': 0x20, 'display': 0, 'graticule_lines': 0, 'scale_labels': 0,
                     'intensity': 255, 'target_scale': 2},
        },
    ]
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


def near(a, b, tol=40):
    'Colors within a tolerance.'
    return all(abs(x - y) <= tol for x, y in zip(a, b))


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
            video = profile['Video']
            video['ColorSpace'] = '2100HLG'
            video['ColorFormat'] = 'P010'
            video['ColorRange'] = 'Partial'
            profile.save()
            notice(f'profile {profile.path}: Video {dict(video)}')
        self.obs.run()

    def log_video_settings(self):
        'Show the video settings OBS reports in its log.'
        log = self.obs.get_logfile()
        if not log:
            return
        with open(log, encoding='utf-8', errors='replace') as fr:
            lines = [l.strip() for l in fr if re.search(r'color space|format:|sdr white|hdr nominal', l, re.I)]
        notice('OBS video: ' + ' | '.join(lines[:8]))

    def screenshot(self, cl, name, repeat=40, size=None):
        'Render the source repeatedly so its pipeline has data, then return the last image.'
        from PIL import Image  # pylint: disable=import-outside-toplevel
        req = {'sourceName': name, 'imageFormat': 'png'}
        if size:
            req['imageWidth'], req['imageHeight'] = size
        data = None
        for _ in range(repeat):
            try:
                data = cl.send('GetSourceScreenshot', req).image_data
            except Exception:  # pylint: disable=broad-except
                data = None
            time.sleep(0.05)
        self.assertTrue(data, f'no screenshot of {name}')
        raw = base64.b64decode(data.split(',', 1)[1])
        img = Image.open(io.BytesIO(raw)).convert('RGB')
        os.makedirs('screenshots', exist_ok=True)
        img.save(f'screenshots/{self.name}-{name}-{int(time.time() * 1000)}.png')
        return img

    def source_of(self, cl, kind):
        'Name of the dock source of a kind.'
        names = [i['inputName'] for i in cl.send('GetInputList').inputs if i['inputKind'].startswith(PREFIX + kind)]
        self.assertTrue(names, f'no {kind} source')
        return names[0]

    def measure(self, cl, what):
        'Center color of the false color scope, with the waveform profile for reference.'
        fc = self.screenshot(cl, self.source_of(cl, 'falsecolor_source'))
        color = fc.getpixel((fc.width // 2, fc.height // 2))
        wv = self.screenshot(cl, self.source_of(cl, 'waveform_source'), repeat=10, size=(256, 256))
        totals = [sum(sum(wv.getpixel((x, y))) for x in range(0, wv.width, 4)) for y in range(wv.height)]
        lit = [f'{y}:{t}' for y, t in enumerate(totals) if t > 0][:10]
        notice(f'{what}: false color center {color} (size {fc.size}); waveform lit rows {lit}')
        return color

    def add_white(self, cl):
        'A white color source over the whole picture.'
        video = cl.send('GetVideoSettings')
        cl.send('CreateInput', {
            'sceneName': 'Scene', 'inputName': 'White', 'inputKind': 'color_source_v3',
            'inputSettings': {'color': 0xFFFFFFFF, 'width': video.base_width, 'height': video.base_height},
        })


class SDRProjectTest(HLGTestBase):
    'SDR project: Rec. 709 versus HLG chosen for the dock'

    def test_sdr_and_forced_hlg(self):
        'White clips at 100 % in Rec. 709 and lands in the highlights when HLG is chosen'
        cl = self.obs.get_obsws()
        self.add_white(cl)
        color = self.measure(cl, 'SDR project, Auto')
        self.assertTrue(near(color, CLIP), f'white is not at 100 % in Rec. 709: {color}')

        cl.send('SetInputSettings', {'inputName': self.source_of(cl, 'roi'), 'inputSettings': {'colorspace': 3}})
        color = self.measure(cl, 'SDR project, HLG chosen')
        self.assertTrue(near(color, HIGHLIGHT), f'white is not at the HLG level of SDR white: {color}')


class HLGProjectTest(HLGTestBase):
    'OBS project in Rec. 2100 HLG: the dock follows it automatically'
    hdr_project = True

    def test_hlg_project(self):
        'White from the HDR canvas lands in the highlights'
        cl = self.obs.get_obsws()
        self.log_video_settings()
        self.add_white(cl)
        color = self.measure(cl, 'HLG project, Auto')
        self.assertTrue(near(color, HIGHLIGHT), f'white is not at the HLG level of SDR white: {color}')


if __name__ == '__main__':
    unittest.main()
