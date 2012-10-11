import os
from ez_setup import use_setuptools

use_setuptools()
from setuptools import setup
from distutils.core import Extension

gemtools_dir = "../../GEMTools/"
objs = []
for f in os.listdir(gemtools_dir):
    if f.endswith(".o"):
        objs.append(gemtools_dir+f)



gemtools = Extension('gem.gemtools',
                    define_macros=[('MAJOR_VERSION', '1'),
                                   ('MINOR_VERSION', '1')],
                    include_dirs=['../../GEMTools'],
                    extra_objects=objs,
                    sources=['src/py_iterator.c', 'src/py_template_iterator.c',
                               'src/py_mismatch.c', 'src/py_map.c', 'src/py_alignment.c',
                               'src/py_template.c', 'src/gemtoolsmodule.c', 'src/py_mappings_iterator.c'])

gem_binaries = "../../binaries/gem-2.2"

setup(
        name='Gem',
        version='1.2',
        description='Python support library for the GEM mapper and gemtools',
        author='Thasso Griebel',
        author_email='thasso.griebel@gmail.com',
        url='http://barnaserver.com/gemtools',
        long_description='''
        This is the python binding to the gemtools library.
        ''',
        packages=['gem'],
        data_files=[("gem/gem-binaries/", ["%s/%s" % (gem_binaries,x) for x in os.listdir(gem_binaries)])],
        ext_modules=[gemtools],
        setup_requires=['nose>=1.0'],
        test_suite = 'nose.collector',
)
