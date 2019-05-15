#include "pollyintegration.h"
#include <common/downloadmanager.h>
#include <QTemporaryFile>

PollyIntegration::PollyIntegration(DownloadManager* dlManager):
    _dlManager(dlManager),
    nodePath(""),
    jsPath(""),
    _hasIndexFile(false),
    _fPtr(nullptr),
    _retries(0)
{
    credFile = QStandardPaths::writableLocation(QStandardPaths::QStandardPaths::GenericConfigLocation) + QDir::separator() + "cred_file";

    // It's important to look for node in the system first, as it might not always be present in the bin dir.
     nodePath = QStandardPaths::findExecutable("node");

    #ifdef Q_OS_WIN
      if(!QStandardPaths::findExecutable("node", QStringList() << qApp->applicationDirPath()).isEmpty())
        nodePath = qApp->applicationDirPath() + QDir::separator() + "node.exe";
    #endif

    #ifdef Q_OS_LINUX
      if(!QStandardPaths::findExecutable("node", QStringList() << qApp->applicationDirPath()).isEmpty())
          nodePath = qApp->applicationDirPath() + QDir::separator() + "node";
    #endif

    #ifdef Q_OS_MAC
      QString binDir = qApp->applicationDirPath() + QDir::separator() + ".." + QDir::separator() + ".." + QDir::separator() + ".." + QDir::separator();
      if(!QStandardPaths::findExecutable("node", QStringList() << binDir + "node_bin" + QDir::separator() ).isEmpty())
        nodePath = binDir + "node_bin" + QDir::separator() + "node";
    #endif

     _dlManager->setRequest("https://raw.githubusercontent.com/ElucidataInc/polly-cli/master/prod/index.js", this);
}

void PollyIntegration::requestSuccess()
{
    jsPath = qApp->applicationDirPath() + QDir::separator() + "_index.js";
    #ifdef Q_OS_MAC
      QString binDir = qApp->applicationDirPath() + QDir::separator() + ".." + QDir::separator() + ".." + QDir::separator() + ".." + QDir::separator();
      jsPath = binDir + "_index.js";
    #endif

    _fPtr = new QTemporaryFile(jsPath);
    if(_fPtr->open()) {
        _fPtr->write(_dlManager->getData());
        qDebug() << "data written to file: " << _fPtr->fileName();
        jsPath = _fPtr->fileName();
        _hasIndexFile = true;
    } else {
        jsPath = "";
        _hasIndexFile = false;
        qDebug() << "unable to open temp file: " << _fPtr->errorString();
    }
    _fPtr->close();
}

void PollyIntegration::requestFailed()
{
    jsPath = "";
    _hasIndexFile = false;
    qDebug() << "failed to download file";
}

PollyIntegration::~PollyIntegration()
{
    if(_fPtr != nullptr) {
        if(_fPtr->remove())
            qDebug() << "removed file";
        else {
            qDebug() << "error: " << _fPtr->error();
            qDebug() << _fPtr->errorString();
        }
        delete _fPtr;
    }
    qDebug()<<"exiting PollyIntegration now....";
}

QString PollyIntegration::getCredFile(){
    return credFile;
}

void PollyIntegration::checkForIndexFile()
{
    if((_fPtr && !_fPtr->exists()) || !_hasIndexFile) {
        // try fetchting the index file once
        if(_retries < 1) {
            qDebug() << "Index file not found, trying to download index file ";
            // synchronous request;
            _dlManager->setRequest("https://raw.githubusercontent.com/ElucidataInc/polly-cli/master/prod/index.js", this, false);
            if(!_dlManager->err) {
                requestSuccess();
            } else {
                requestFailed();
            }
            qDebug() << "Request status: " << _hasIndexFile;
        }
    } else {
        qDebug() << "index file exists : "<< _fPtr->fileName();
        qDebug() << "js path: " << jsPath;
    }
    _retries++;
}

QList<QByteArray> PollyIntegration::runQtProcess(QString command, QStringList args){

    // e.g: command = "authenticate", "get_Project_names" etc
    // e.g: args = username, password, projectName  etc

    checkForIndexFile();
    if(jsPath == "")
        return QList<QByteArray>();

    QProcess process;
    QStringList arg;
    qDebug() << "js file being used: " << jsPath;
    arg << jsPath; // where index.js files is placed
    arg << command; // what command to pass to index.js. eg. authenticate
    arg << args; // required params by that command . eg username, password
    // qDebug()<<"nodePath"<<nodePath;
    // qDebug()<<"args -"<<arg;

    // nodePath = "PATH_OF_MAVEN/bin/node.exe"
    process.setProgram(nodePath);
    process.setArguments(arg);

    process.start();
    //TODO kailash, use threading for this, it should not just run indefinitely 
    process.waitForFinished(-1);
    QByteArray result = process.readAllStandardOutput();
    QByteArray result2 = process.readAllStandardError();
    qDebug()<<"StandardOutput  - "<<result;
    qDebug()<<"StandardError, if any  - "<<result2;
    return QList<QByteArray>()<<result<<result2;
}

QString PollyIntegration::parseId(QByteArray result){
    QList<QByteArray> test_list = result.split('\n');
    int size = test_list.size();
    QByteArray result2 = test_list[size-2];
    QJsonDocument doc(QJsonDocument::fromJson(result2));
    // Get JSON object
    QJsonObject json = doc.object();
    QVariantMap json_map = json.toVariantMap();
    QString run_id =  json_map["id"].toString();
    return run_id;
}

QMap<QString, QStringList> PollyIntegration::_fetchAppLicense() {
    QString command = "fetchAppLicense";
    QList<QByteArray> resultAndError = runQtProcess(command,
                                                    QStringList() << credFile);

    QByteArray result = resultAndError.at(0);
    QStringList resultList = QString::fromStdString(result.toStdString()).split('\n');
    for (int i = 0; i < resultList.length(); i++) {
        auto temp = resultList[i];
        if (temp.contains("components")) {
            temp = resultList[++i];
            _appLicenses["components"] = temp.replace(' []', "").split(',');
        }
        if (temp.contains("workflows")) {
            temp = resultList[++i];
            _appLicenses["workflows"] = temp.replace(' []', "").split(',');
        }
        if (temp.contains("license")) {
            temp = resultList[++i];
            _appLicenses["licenseActive"] = temp.replace(' []', "").split(',');
        }
    }

    return _appLicenses;
}

QString PollyIntegration::obtainComponentId(QString componentName)
{
    QString command = "getComponentId";
    QList<QByteArray> resultAndError = runQtProcess(command,
                                                    QStringList() << credFile);
    QByteArray result = resultAndError.at(0);

    // replace doubly escaped quotes
    result = result.replace("\\\"", "\"");

    // split based on newlines, JSON is second last string after split
    QList<QByteArray> resultList = result.split('\n');
    result = resultList[resultList.size() - 2];

    // remove extra quotation around JSON array
    result = result.right(result.size() - 1);
    result = result.left(result.size() - 1);

    QJsonDocument doc(QJsonDocument::fromJson(result));
    auto array = doc.array();
    for (auto elem : array) {
        auto jsonObject = elem.toObject();
        auto name = jsonObject.value("component").toString();
        if (name == componentName)
            return QString::number(jsonObject.value("id").toInt());
    }
    return QString::number(-1);
}

QStringList PollyIntegration::get_project_upload_url_commands(QString url_with_wildcard, 
                                QStringList filenames) {
    
    QStringList patchId;
    for (auto& filename : filenames) {
        QStringList test_files_list = filename.split(QDir::separator());
        int size = test_files_list.size();
        QString new_filename = test_files_list[size-1];
        QString copy_url_with_wildcard = url_with_wildcard;
        QString url_map_json = copy_url_with_wildcard.replace("*", new_filename) ;
        QString upload_command = "upload_project_data";
        QList<QByteArray> patch_id_result_and_error = runQtProcess(upload_command, QStringList() << url_map_json << filename);
        patchId.append(patch_id_result_and_error.at(0));
    }
    return patchId;
}

// name OF FUNCTION: get_projectFiles_download_url_commands
// PURPOSE:
//    This function parses the output of "createproject" command run from Qtprocess..
// Return project id for the new project..
// CALLS TO: runQtProcess
//
// CALLED FROM: loadDataFromPolly

QStringList PollyIntegration::get_projectFiles_download_url_commands(QByteArray result2,QStringList filenames){
    QStringList patchId ;
    QList<QByteArray> test_list = result2.split('\n');
    int size = test_list.size();
    QByteArray url_jsons = test_list[size-2];
    QJsonDocument doc(QJsonDocument::fromJson(url_jsons));
    // Get JSON object
    QJsonObject json = doc.object();
    QVariantMap json_map = json.toVariantMap();
    for (int i=0; i < filenames.size(); ++i){
        QString filename = filenames.at(i);
        QStringList test_files_list = filename.split(QDir::separator());
        int size = test_files_list.size();
        QString new_filename = test_files_list[size-1];
        QString url_with_wildcard =  json_map["file_upload_urls"].toString();
        QString url_map_json = url_with_wildcard.replace("*",new_filename) ;
        QString upload_command = "download_project_data";
        QList<QByteArray> result_and_error = runQtProcess(upload_command,QStringList() <<url_map_json <<filename);
        patchId.append(result_and_error.at(0));
    }
    return patchId;
}

bool PollyIntegration::activeInternet()
{
    QString command = QString("check_internet_connection");
    QList<QByteArray> results = runQtProcess(command, QStringList());
    if(!results.isEmpty()) {
        results = results.at(0).split('\n');
        QString status = results[1];
        if (status == "Connected") {
            return true;
        }
    }
    return false; 
}

// name OF FUNCTION: checkLoginStatus
// PURPOSE:
//    This function checks if the user is already logged in or not.
// Returns log in status of current user

// CALLS TO: runQtProcess
//
// CALLED FROM: authenticateLogin

int PollyIntegration::checkLoginStatus(){
    int status;
    QString command = QString("authenticate");
    QList<QByteArray> resultAndError = runQtProcess(command, QStringList() << credFile);
    QList<QByteArray> testList = resultAndError.at(0).split('\n');
    QByteArray statusLine = testList[0];
    if (statusLine == "already logged in") {
        status = 1;

        // logged in successfully, there must be a user email
        QByteArray userline = testList[1];
        QList<QByteArray> split = userline.split(' ');
        _username = QString(split.back());
    }
    else {
        status = 0;
    }
    return status;
}

// void PollyIntegration::checkLicense() {
//     qDebug() << "checking license";
//     QString command = "getLicense";
//     QString status;

//     QList<QByteArray> resultAndError = runQtProcess(command, QStringList() << credFile);
//     QByteArray result = resultAndError.at(0);

//     // split based on newlines, JSON is second last string after split
//     QList<QByteArray> resultList = result.split('\n');
//     result = resultList[resultList.size() - 1];

//     // remove extra quotation around JSON array
//     result = result.right(result.size() - 1);
//     //result = result.left(result.size() - 1);
//     // replace doubly escaped quotes
//     result = result.replace("\\\"", "\"");


//     qDebug() << result;

//     QJsonDocument doc(QJsonDocument::fromJson(result));
//     QJsonObject jsonObj = doc.object()["licenses"].toObject();
//     auto licenses = jsonObj.keys();
//     auto component = jsonObj["components"].toArray();
//     if (component.size())
//         qDebug() << component;

//     qDebug() << "json error" << QString::number(-1);
// }

// name OF FUNCTION: authenticateLogin
// PURPOSE:
//    This function is responsible for authenticating the user.
// This is the entry point for Elmaven-Polly-Integration
//It will first run the command "authenticate" and then check its ouptut to see if log in was succesfull
// username and password are inputed from external clients
// Returns status of log in commands, if successfully logged in, it will return 1, else 0
// CALLS TO: runQtProcess,checkLoginStatus
//
// CALLED FROM: external clients


QString PollyIntegration::authenticateLogin(QString username, QString password) {
    QString command = "authenticate";
    QString status;
    
    QList<QByteArray> resultAndError = runQtProcess(command, QStringList() << credFile << username << password);
    int statusInside = checkLoginStatus();
    if (statusInside == 1) {
        status = "ok";
    }
    else if (resultAndError.at(1) != "") {
        status = "error";
    }
    else {
        status = "incorrect credentials";
    }
    QMap<QString, QStringList> _fetchAppLicense();
    return status;
}

QString PollyIntegration::getCurrentUsername()
{
    return _username;
}

// This function checks if node executable path has been defined for the library or not..
int PollyIntegration::checkNodeExecutable() {
    if (nodePath == "") {
        return 0;
    }
    return 1;
    
}

int PollyIntegration::askForLogin() {
    qDebug() << "credFile  -\n" << credFile;
    QFile file (credFile);
    QFile refreshTokenFile (credFile + "_refreshToken");
    if (file.exists() && refreshTokenFile.exists()) {
        qDebug() << "both tokens exist.. moving on to refresh now..";
        return 0;
    }
    qDebug() <<"both tokens do not exist.. moving on to login now..";
    return 1;
}

// This function deletes the token and logs out the user..
void PollyIntegration::logout() {
    _username = "";
    QFile file (credFile);
    file.remove();
    QFile refreshTokenFile (credFile + "_refreshToken");
    refreshTokenFile.remove();
}
// name OF FUNCTION: getUserProjectsMap
// PURPOSE:
//    This function parses the output of "get_Project_names" command and store it in a json format to be used later..
// When the user selects a project on Elmaven GUI, this json will be used to get ID for that project
// This ID will then further be used for uploading and all..

// Returns user projects in json format with id as keys and name as values..
// CALLS TO: 
//
// CALLED FROM: getUserProjects

QVariantMap PollyIntegration::getUserProjectsMap(QByteArray result2) {
    QVariantMap userProjects;
    QList<QByteArray> testList = result2.split('\n');
    int size = testList.size();
    QByteArray resultJsons = testList[size-2];
    QJsonDocument doc(QJsonDocument::fromJson(resultJsons));
    // Get JSON object
    QJsonArray jsonArray = doc.array();
    for (int i = 0; i < jsonArray.size(); ++i){
        QJsonValue projectJson = jsonArray.at(i);
        QJsonObject projectJsonObject = projectJson.toObject();
        QVariantMap projectJsonObjectMap = projectJsonObject.toVariantMap();
        userProjects[projectJsonObjectMap["id"].toString()] = projectJsonObjectMap["name"].toString();
    }
    return userProjects;
}

// name OF FUNCTION: getUserProjects
// PURPOSE:
//    This function runs "get_Project_names" command from Qtprocess..
// This command will return data corresponding to all the projects on polly for the logged in user..
// This function will then call getUserProjectsMap to parse the output and store it in a json format to be used later..
// When the user selects a project on Elmaven GUI, this json will be used to get ID for that project
// This ID will then further be used for uploading and all..

// Returns user projects in json format with id as keys and name as values..
// CALLS TO: getUserProjects
//
// CALLED FROM: 


QVariantMap PollyIntegration::getUserProjects() {
    QString getProjectsCommand = "get_Project_names";
    QList<QByteArray> resultAndError = runQtProcess(getProjectsCommand, QStringList() << credFile);
    QVariantMap userProjects = getUserProjectsMap(resultAndError.at(0));
    return userProjects;
}

// name OF FUNCTION: getUserProjectFilesMap
// PURPOSE:
//    This function parses the output of "get_Project_files" command run from Qtprocess..

// Return a map of projectid and files uploaded to that project on polly..
// CALLS TO: getUserProjects
//
// CALLED FROM: getUserProjectFiles


QStringList PollyIntegration::getUserProjectFilesMap(QByteArray result2) {
    QStringList userProjectfiles;
    QList<QByteArray> testList = result2.split('\n');
    int size = testList.size();
    QByteArray resultJsons = testList[size-2];
    QJsonDocument doc(QJsonDocument::fromJson(resultJsons));
    // Get JSON object
    QJsonObject projectJsonObject = doc.object();
    QVariantMap projectJsonObjectMap = projectJsonObject.toVariantMap();
    userProjectfiles = projectJsonObjectMap["project_files"].toStringList();
    return userProjectfiles;
}

// name OF FUNCTION: getUserProjectFiles
// PURPOSE:
//    This function downloads the file information for all the projects corresponding to the logged in user.
// First of all it will run "get_Project_files" command which will connect to polly through index.js
//and then this function will call  getUserProjectFilesMap to parse the output of that command

// Returns a map of projectid and files uploaded to that project on polly..
// CALLS TO: getUserProjectFilesMap
//
// CALLED FROM: external clients

QVariantMap PollyIntegration::getUserProjectFiles(QStringList projectIds) {
    QVariantMap userProjectfilesmap;
    for (int i=0; i < projectIds.size(); ++i){
        QString projectId = projectIds.at(i);
        QString getProjectsCommand = "get_Project_files";
        QList<QByteArray> resultAndError = runQtProcess(getProjectsCommand, QStringList() << credFile << projectId);
        QStringList userProjectfiles = getUserProjectFilesMap(resultAndError.at(0));
        userProjectfilesmap[projectId] = userProjectfiles;
    }
    return userProjectfilesmap;
}

// name OF FUNCTION: createProjectOnPolly
// PURPOSE:
//    This function parses the output of "createproject" command run from Qtprocess..
// Return project id for the new project..
// CALLS TO: runQtProcess,parseId
//
// CALLED FROM: external clients


QString PollyIntegration::createProjectOnPolly(QString projectname) {
    QString command2 = "createProject";
    QList<QByteArray> resultAndError = runQtProcess(command2, QStringList() << credFile << projectname);
    QString runId = parseId(resultAndError.at(0));
    return runId;
}

QString PollyIntegration::createWorkflowRequest(QString projectId){
    QString command2 = "createWorkflowRequest";
    QList<QByteArray> resultAndError = runQtProcess(command2, QStringList() << credFile << projectId);
    QString workflowRequestId = parseId(resultAndError.at(0));
    return workflowRequestId;
}

QString PollyIntegration::createRunRequest(QString componentId,
                                           QString projectId)
{
    QString command = "createRunRequest";
    QStringList arguments = QStringList() << credFile
                                          << componentId
                                          << projectId;
    QList<QByteArray> resultAndError = runQtProcess(command, arguments);
    QString runId = parseId(resultAndError.at(0));
    return runId;
}

QString PollyIntegration::redirectionUiEndpoint(QString componentId,
                                                QString runId,
                                                QString datetimestamp)
{
    QString command = "getEndpointForRuns";
    QList<QByteArray> resultAndError = runQtProcess(command, QStringList(credFile));

    QByteArray result = resultAndError.at(0);

    // split based on newlines, JSON is second last string after split
    QList<QByteArray> resultList = result.split('\n');
    result = resultList[resultList.size() - 2];

    QJsonDocument doc(QJsonDocument::fromJson(result));
    auto array = doc.array();
    for (auto elem : array) {
        auto jsonObject = elem.toObject();
        auto id = QString::number(jsonObject.value("component_id").toInt());
        if (id == componentId) {
            auto url = jsonObject.value("url").toString();
            url = url.replace("<runid>", runId)
                     .replace("<timestamp>", datetimestamp);
            return url;
        }
    }
    return "";
}

bool PollyIntegration::sendEmail(QString userEmail,
                                 QString emailMessage,
                                 QString emailContent,
                                 QString appName)
{
    QString command2 = "send_email";
    qDebug() << userEmail << emailMessage << emailContent << appName;
    QList<QByteArray> resultAndError = runQtProcess(command2,
                                                    QStringList() << userEmail
                                                                  << emailMessage
                                                                  << emailContent
                                                                  << appName);
    QList<QByteArray> testList = resultAndError.at(0).split('\n');
    int size = testList.size();
    if (size > 2) {
        QByteArray result2 = testList[size-2];
        if (result2 == "1")
            return true;
    }

    return false;
}

// name OF FUNCTION: exportData
// PURPOSE:
//    This function uploads specified files to the given project
//First this function will run "get_upload_Project_urls" command which provide the upload url..
//then this function will call get_project_upload_url_commands function to upload all the given files to that url.. 
// Return patch ids for all the uploads..
// CALLS TO: runQtProcess,get_project_upload_url_commands
//
// CALLED FROM: external clients


QStringList PollyIntegration::exportData(QStringList filenames, QString projectId) {
    qDebug() << "files to be uploaded " << filenames;
    QElapsedTimer timer;
    timer.start();
    QString get_upload_Project_urls = "get_upload_Project_urls";
    QList<QByteArray> result_and_error = runQtProcess(get_upload_Project_urls, QStringList() << credFile << projectId);
    QString url_with_wildcard = getFileUploadURLs(result_and_error.at(0));    
    QStringList patchId = get_project_upload_url_commands(url_with_wildcard, filenames);
    qDebug() << "time taken in uploading json file, by polly cli is - " << timer.elapsed();
    
    return patchId;
}

QString PollyIntegration::getFileUploadURLs(QByteArray result2) {
    QList<QByteArray> test_list = result2.split('\n');
    int size = test_list.size();
    QByteArray url_jsons = test_list[size-2];
    QJsonDocument doc(QJsonDocument::fromJson(url_jsons));
    // Get JSON object
    QJsonObject json = doc.object();
    QVariantMap json_map = json.toVariantMap();
    QString url_with_wildcard =  json_map["file_upload_urls"].toString();

    return url_with_wildcard;
}

QString PollyIntegration::UploadToCloud(QString uploadUrl, QString filePath){
    QString upload_command = "uploadCuratedPeakDataToCloud";
    QList<QByteArray> patch_id_result_and_error = runQtProcess(upload_command, QStringList() << uploadUrl << filePath);
    QString status = "success";
    return status;
}

QString PollyIntegration::UploadPeaksToCloud(QString session_indentifier, QString fileName, QString filePath){
    QElapsedTimer timer;
    timer.start();
    QString command = "getPeakUploadUrls";
    QList<QByteArray> result_and_error = runQtProcess(command, QStringList() << session_indentifier << fileName);
    QString uploadUrl = getFileUploadURLs(result_and_error.at(0));
    QString status = UploadToCloud(uploadUrl, filePath);
    qDebug() << "time taken in uploading json file, by polly cli is - " << timer.elapsed();
    return status;
}

// name OF FUNCTION: loadDataFromPolly
// PURPOSE:
//    This function downloads the specified files for a given projects..
// External clients can then use those files to load data back to Elmaven..

// CALLS TO: runQtProcess,get_projectFiles_download_url_commands
//
// CALLED FROM: external clients



QString PollyIntegration::loadDataFromPolly(QString ProjectId,QStringList filenames) {
    QString get_upload_Project_urls = "get_upload_Project_urls";
    QList<QByteArray> result_and_error = runQtProcess(get_upload_Project_urls, QStringList() << credFile << ProjectId);
    QStringList patchId = get_projectFiles_download_url_commands(result_and_error.at(0),filenames);
    if (0<filenames.size()&&!patchId.isEmpty()){
        return "project data loaded";
    }
    else if(0<filenames.size()&&patchId.isEmpty()){
        return "error while loading project";
    }
    else{
        return "no file selected to laod";
    }
}

bool PollyIntegration::validSampleCohort(QString sampleCohortFile, QStringList loadedSamples) {
	qDebug() << "Validating sample cohort file now";
	
    QFile file(sampleCohortFile);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << file.errorString();
		return false;
    }

    QStringList samples;
	QStringList cohorts;
    while (!file.atEnd()) {
        QByteArray line = file.readLine();
		QList<QByteArray> splitRow = line.split(',');
		if (splitRow.size() != 2) {
            qDebug() << "Missing column(s)";
			return false;
        }
		
		//skip header row
		if (splitRow.at(0) == "Sample")
			continue;
		
		QString sampleName = splitRow.at(0);
		QString cohortName = splitRow.at(1);
        
        if (cohortName.trimmed().isEmpty()) {
            qDebug() << "Cohort missing for some samples";
            return false;
        }

		samples.append(QString::fromStdString(sampleName.toStdString()));
		cohorts.append(QString::fromStdString(cohortName.toStdString()));
    }

	//this check is only required when user provides a sample cohort file through cli
    if (!loadedSamples.isEmpty()) {
        qSort(samples);
	    qSort(loadedSamples);
	
	    if (!(samples == loadedSamples)) {
		    qDebug() << "The sample cohort file contains different sample names than the samples loaded in Elmaven...Please try again with the correct file" << endl;
		    return false;
	    }                               
    }
	
	if (!validCohorts(cohorts)) {
		qDebug() << "The sample cohort file contains more than 9 cohorts. As of now, Polly supports only 9 or less cohorts..please try again with the correct file";
		return false;
	}

	return true;
}

bool PollyIntegration::validCohorts(QStringList cohorts) {
	cohorts.removeDuplicates();
	if(cohorts.size() > 9)
		return false;
	
	return true;
}

QStringList PollyIntegration::parseResultOrganizationalDBs(QString result){
    QStringList OrganizationalDBs;

    return OrganizationalDBs;
}
QStringList PollyIntegration::getOrganizationalDBs(QString organization){
    QString command = "get_organizational_databases";
    QList<QByteArray> result_and_error = runQtProcess(command, QStringList() << credFile << organization);
    QStringList OrganizationalDBs = parseResultOrganizationalDBs(result_and_error.at(0));    
    return OrganizationalDBs;
}
