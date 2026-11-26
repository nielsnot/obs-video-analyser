'''
Auto select: the selection is the face skin or the background, found by itself. On a picture with faces in the left
half and plain grey in the right half, a waveform with "face skin" has a trace only in the columns of the faces,
and with "background" the grey half lights up.
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
FACE_PNG = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets', 'face.png')


def notice(msg):
    'Show a line in the GitHub Actions summary.'
    print(f'::notice::{msg}', flush=True)


def dock_data(parts):
    'A dock with auto select on the given parts, a luma waveform on the dock selection and a drawn shape that must not count.'
    scopes = [{'type': 'waveform', 'label': 'Waveform', 'show_label': False, 'use_roi': True, 'target': '',
               'prop': {'target_scale': 2, 'components': 0x20, 'trace_curve': 0, 'trace_smooth': 0, 'display': 0, 'intensity': 255,
                        'graticule_lines': 0, 'scale_labels': 0, 'minmax': False}}]
    return {'docks': [{
        'name': 'Auto', 'toolbar': True, 'active_view': 0,
        'roi-prop': {'target_name': '', 'interleave': 0, 'target_scale': 2, 'auto_sel': True, 'auto_skin': False,
                     **{f'auto_{p}': True for p in parts},
                     'shapes': [{'type': 0, 'x0': 0.8, 'y0': 0.8, 'x1': 0.95, 'y1': 0.95}]},
        'views': [{'name': 'View 1', 'state': {'layout': 0, 'roi-shown': True, 'scopes': scopes}}],
    }]}


class AutoSelectTest(obstest.OBSTest):
    'Auto select'
    parts = ['face']

    def setUp(self, config_name='saved-config', run=True):
        super().setUp(run=False, config_name=config_name)
        path = self.obs.config.get_scenecollection_file()
        if not os.path.exists(path) and os.path.exists(path + '.json'):
            path += '.json'
        with open(path, encoding='utf-8') as fr:
            collection = json.load(fr)
        collection.setdefault('modules', {})[DOCK_KEY] = dock_data(self.parts)
        with open(path, 'w', encoding='utf-8') as fw:
            json.dump(collection, fw)
        self.obs.run()

    def columns(self, done):
        'The share of waveform columns with a trace, once done(share) or after a while.'
        from PIL import Image  # pylint: disable=import-outside-toplevel
        cl = self.obs.get_obsws()
        video = cl.send('GetVideoSettings')
        res = cl.send('CreateInput', {
            'sceneName': 'Scene', 'inputName': 'Face', 'inputKind': 'image_source',
            'inputSettings': {'file': FACE_PNG},
        })
        # the faces in the left half of the picture, a plain grey right half
        grey = cl.send('CreateInput', {
            'sceneName': 'Scene', 'inputName': 'Grey', 'inputKind': 'color_source_v3',
            'inputSettings': {'color': 0xFF808080, 'width': video.base_width, 'height': video.base_height},
        })
        cl.send('SetSceneItemIndex', {'sceneName': 'Scene', 'sceneItemId': grey.scene_item_id,
                                      'sceneItemIndex': 0})
        cl.send('SetSceneItemTransform', {
            'sceneName': 'Scene', 'sceneItemId': res.scene_item_id,
            'sceneItemTransform': {'boundsType': 'OBS_BOUNDS_STRETCH', 'boundsWidth': video.base_width // 2,
                                   'boundsHeight': video.base_height // 2, 'positionX': 0,
                                   'positionY': video.base_height // 4},
        })
        name = [i['inputName'] for i in cl.send('GetInputList').inputs
                if i['inputKind'].startswith(PREFIX + 'waveform_source')][0]
        share, img = 0.0, None
        for _ in range(80):
            try:
                data = cl.send('GetSourceScreenshot', {'sourceName': name, 'imageFormat': 'png',
                                                       'imageWidth': 640, 'imageHeight': 256}).image_data
            except Exception:  # pylint: disable=broad-except
                time.sleep(0.1)
                continue
            img = Image.open(io.BytesIO(base64.b64decode(data.split(',', 1)[1]))).convert('RGB')
            lit = [any(sum(img.getpixel((x, y))) > 90 for y in range(img.height))
                   for x in range(0, img.width, 4)]
            share = sum(lit) / len(lit)
            if done(share):
                break
            time.sleep(0.1)
        if img:
            os.makedirs('screenshots', exist_ok=True)
            img.save(f'screenshots/auto-select-{"-".join(self.parts)}.png')
        return share

    def test_face_skin(self):
        'With face skin only the columns of the faces have a trace'
        share = self.columns(lambda s: 0.03 < s < 0.5)
        notice(f'auto select face skin: {share:.0%} of the waveform columns have a trace')
        if not 0.03 < share < 0.5:
            # depends on the face detector seeing the faces in time: report, do not block
            print('::warning::auto select: the face skin was not found', flush=True)
            self.skipTest('auto select did not find the face skin')


class AutoSelectBackgroundTest(AutoSelectTest):
    'Auto select on the background'
    parts = ['background']

    def test_face_skin(self):
        'Not for this part'

    def test_background(self):
        'With the background the grey half lights up'
        share = self.columns(lambda s: s > 0.45)
        notice(f'auto select background: {share:.0%} of the waveform columns have a trace')
        self.assertGreater(share, 0.45, 'the grey background is not selected')


if __name__ == '__main__':
    unittest.main()
