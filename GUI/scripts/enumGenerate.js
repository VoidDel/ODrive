const fs = require('fs');
const path = require('path');


fs.readFile(path.resolve(__dirname,'..','..','tools','odrive','enums.py'), 'utf8', function(err, data) {
    if (err) throw err;
    let enumsString = data;
    let lines = enumsString.split(process.platform == 'win32' ? '\r\n' : '\n');
    let enums = {};
    for (const line of lines) {
        if (line != '' && line[0] != '#' && line.includes('=')) {
            let parts = line.split('=');
            if (parts.length >= 2 && parts[1]) {
                let name = parts[0].trim();
                let value = parts[1].trim();
                if (name && value) {
                    enums[name] = parseInt(value);
                }
            }
        }
    }
    fs.writeFile(path.resolve(__dirname, '../src/assets/odriveEnums.json'), JSON.stringify(enums, null, 4), function(err) {
        if (err) throw err;
    });
    console.log('Wrote ODrive enums to GUI/src/assets/odriveEnums.json');
});
